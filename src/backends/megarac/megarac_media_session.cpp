#include "backends/megarac/megarac_media_session.hpp"

#include "backends/megarac/bmc_ws_framing.hpp"
#include "backends/megarac/megarac_iusb.hpp"
#include "bmc_session.hpp"
#include "diagnostics.hpp"
#include "http_client.hpp"
#include "log.hpp"
#include "backends/megarac/megarac_session.hpp"
#include "virtual_media/block_source.hpp"
#include "virtual_media/iso_file_source.hpp"
#include "virtual_media/scsi_cd_target.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hitsc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace json = boost::json;
namespace ssl = boost::asio::ssl;
namespace websocket = boost::beast::websocket;
using tcp = asio::ip::tcp;

namespace {

constexpr auto kMediaStopGrace = std::chrono::milliseconds(250);
// A sane upper bound on one IUSB packet (a 64-sector READ is 128 KB + framing); anything
// larger means the framing has desynchronized, so we bail rather than buffer forever.
constexpr std::size_t kMaxIusbPacket = 256 * 1024;

std::string json_string_field(const json::object& object, std::string_view name)
{
    const json::value* value = object.if_contains(name);
    if (value == nullptr) {
        return {};
    }
    if (const json::string* string = value->if_string()) {
        return std::string(*string);
    }
    return {};
}

// The /cd-server AUTH token. The H5Viewer sources it from /api/kvm/token (falling back to the
// session info), so try that first and the video path's h5viewercfg token second; whichever
// the firmware accepts wins. (One of the two Phase-2 unknowns -- confirmed live.)
std::string fetch_cd_token(BmcWebSession& web, bool verbose)
{
    std::vector<Header> headers;
    const std::string_view csrf = web.session_token();
    if (!csrf.empty()) {
        headers.push_back(Header{http::field::unknown, "X-CSRFTOKEN", std::string(csrf)});
    }

    for (const char* path : {"/api/kvm/token", "/api/settings/media/h5viewercfg"}) {
        try {
            auto response = web.request(http::verb::get, path, {}, {}, headers);
            if (response.result_int() / 100 != 2) {
                continue;
            }
            boost::system::error_code error;
            const json::value value = json::parse(decode_response_body(response), error);
            if (error || !value.is_object()) {
                continue;
            }
            std::string token = json_string_field(value.as_object(), "token");
            if (!token.empty()) {
                log_info() << "cd-server token via " << path;
                return token;
            }
        } catch (const std::exception& exception) {
            if (verbose) {
                log_warning() << "cd-server token fetch via " << path << " failed: "
                              << exception.what();
            }
        }
    }
    throw std::runtime_error(
        "could not obtain a /cd-server token (tried /api/kvm/token and h5viewercfg)");
}

std::string filename_from_path(const std::string& iso_path)
{
    std::error_code ignored;
    std::filesystem::path path(iso_path);
    std::string name = path.filename().string();
    return name.empty() ? "image.iso" : name;
}

struct OutgoingPacket {
    std::vector<std::uint8_t> bytes;
    std::string encoded_text;  // kept alive across an async base64 write
};

// The async state machine for one /cd-server connection. Structurally a clone of the /kvm
// KvmAsyncSession (strand, read loop with reassembly, a write deque, graceful stop) with the
// IUSB+SCSI dispatch swapped in. Serves SCSI reads from a BlockSource; knows nothing about the
// disk format.
class CdServerAsyncSession : public std::enable_shared_from_this<CdServerAsyncSession> {
public:
    CdServerAsyncSession(
        asio::io_context& io,
        std::shared_ptr<BmcWebSocketStream> ws,
        MegaracViewOptions options,
        std::string token,
        std::string subprotocol,
        std::string filename,
        std::uint8_t cd_device_no,
        BlockSource& source,
        MediaChannel& media)
        : io_(io)
        , strand_(asio::make_strand(io))
        , force_close_timer_(io)
        , ws_(std::move(ws))
        , options_(std::move(options))
        , token_(std::move(token))
        , subprotocol_(std::move(subprotocol))
        , filename_(std::move(filename))
        , cd_device_no_(cd_device_no)
        , source_(source)
        , media_(media)
        , scsi_(source)
    {
    }

    void start()
    {
        auto self = shared_from_this();
        asio::dispatch(strand_, [self] {
            self->media_.publish_state(MediaState::Mounting);
            self->queue_packet(iusb_build_auth(self->token_, self->cd_device_no_));
            self->queue_packet(iusb_build_device_info(self->filename_));
            self->start_read();
        });
    }

    void request_stop()
    {
        auto self = shared_from_this();
        asio::post(strand_, [self] {
            if (self->closed_ || self->stopping_) {
                return;
            }
            self->stopping_ = true;
            self->queue_packet(iusb_build_control(kIusbOpDisconnect));
            self->arm_force_close_timer();
        });
    }

private:
    void start_read()
    {
        if (closed_ || stopping_) {
            return;
        }
        read_buffer_.clear();
        auto self = shared_from_this();
        ws_->async_read(
            read_buffer_,
            asio::bind_executor(strand_, [self](beast::error_code error, std::size_t bytes) {
                self->on_read(error, bytes);
            }));
    }

    void on_read(beast::error_code error, std::size_t /*bytes*/)
    {
        if (error) {
            handle_read_error(error);
            return;
        }

        try {
            const std::vector<std::uint8_t> message = bmc_ws_message_bytes(read_buffer_, subprotocol_);
            accumulator_.insert(accumulator_.end(), message.begin(), message.end());

            std::size_t offset = 0;
            while (accumulator_.size() - offset >= kIusbHeaderSize) {
                const std::uint32_t body = iusb_data_packet_length(accumulator_.data() + offset);
                const std::size_t total = kIusbHeaderSize + body;
                if (total > kMaxIusbPacket) {
                    throw std::runtime_error("IUSB packet length out of range; framing desynced");
                }
                if (accumulator_.size() - offset < total) {
                    break;  // wait for the rest of this packet
                }
                handle_iusb_packet(accumulator_.data() + offset, total);
                if (closed_) {
                    return;
                }
                offset += total;
            }
            if (offset > 0) {
                accumulator_.erase(accumulator_.begin(), accumulator_.begin() + offset);
            }
        } catch (...) {
            media_.publish_outcome(MediaOutcome{false, "media session protocol error"});
            media_.publish_state(MediaState::Error);
            print_current_exception_with_stack(std::cerr, "cd-server handler");
            close_socket();
            return;
        }

        start_read();
    }

    void handle_read_error(beast::error_code error)
    {
        if (stopping_ || closed_ || error == asio::error::operation_aborted ||
            error == asio::error::bad_descriptor) {
            close_socket();
            return;
        }
        if (error == websocket::error::closed || error == ssl::error::stream_truncated) {
            log_warning() << "cd-server websocket closed";
            close_socket();
            return;
        }
        media_.publish_outcome(MediaOutcome{false, std::string("cd-server read failed: ") + error.message()});
        media_.publish_state(MediaState::Error);
        log_error() << "cd-server error: " << error.message()
                    << " [" << error.category().name() << ':' << error.value() << ']';
        close_socket();
    }

    void handle_iusb_packet(const std::uint8_t* packet, std::size_t size)
    {
        if (size <= kIusbScsiOpcodeIndex) {
            // Dropping a request unanswered can wedge the BMC's USB gadget (which also carries
            // the HID devices), so a drop must never be silent.
            log_warning() << "cd-server: dropping " << size
                          << "-byte IUSB packet (too short to carry an opcode); no response sent";
            return;
        }
        const std::uint8_t opcode = packet[kIusbScsiOpcodeIndex];
        if (opcode >= kIusbOpAck) {
            handle_control(opcode, packet, size);
        } else {
            handle_scsi(packet, size);
        }
    }

    void handle_control(std::uint8_t opcode, const std::uint8_t* packet, std::size_t size)
    {
        switch (opcode) {
        case kIusbOpAck:
            handle_ack(packet, size);
            break;
        case kIusbOpKeepAlive:
            // Logged at info so an idle mount's keep-alive handshake is visible without --vverbose.
            log_info() << "cd-server keep-alive received; echoing";
            queue_packet(iusb_build_control(kIusbOpKeepAlive));
            break;
        case kIusbOpDisconnect:
        case kIusbOpKillRedir:
            log_info() << "cd-server redirection ended by host (opcode " << static_cast<int>(opcode) << ")";
            close_socket();
            break;
        default:
            log_warning() << "cd-server: ignoring unknown control opcode "
                          << static_cast<int>(opcode) << " (" << size << " bytes)";
            break;
        }
    }

    void handle_ack(const std::uint8_t* packet, std::size_t size)
    {
        if (ack_seen_) {
            return;
        }
        ack_seen_ = true;

        const std::uint8_t status = size > kIusbAckStatusIndex ? packet[kIusbAckStatusIndex] : 0;
        switch (status) {
        case kIusbConnAccepted:
        case kIusbConnAcceptedBoost:
        case kIusbConnAcceptedNoBoost: {
            const char* boost = status == kIusbConnAcceptedBoost      ? " (media boost)"
                                : status == kIusbConnAcceptedNoBoost  ? " (no media boost)"
                                                                      : "";
            media_.publish_state(MediaState::Mounted);
            media_.publish_outcome(MediaOutcome{true, "Mounted " + filename_ + boost});
            log_info() << "cd-server redirection accepted (status " << static_cast<int>(status)
                       << ")" << boost;
            break;
        }
        case kIusbConnInUse: {
            std::string other;
            for (std::size_t i = kIusbAckOtherIpIndex;
                 i < size && i < kIusbAckOtherIpIndex + kIusbAckOtherIpMaxLen && packet[i] != 0; ++i) {
                other.push_back(static_cast<char>(packet[i]));
            }
            media_.publish_outcome(MediaOutcome{
                false, other.empty() ? "Media redirection already in use"
                                     : "Media redirection already in use by " + other});
            media_.publish_state(MediaState::Error);
            log_warning() << "cd-server in use" << (other.empty() ? "" : " by " + other);
            close_socket();
            break;
        }
        default:
            media_.publish_outcome(MediaOutcome{
                false, "Media redirection rejected (status " + std::to_string(status) + ")"});
            media_.publish_state(MediaState::Error);
            log_error() << "cd-server rejected: status " << static_cast<int>(status);
            close_socket();
            break;
        }
    }

    void handle_scsi(const std::uint8_t* packet, std::size_t size)
    {
        if (size < kIusbDataIndex) {
            log_warning() << "cd-server: dropping " << size
                          << "-byte SCSI packet (command region incomplete, opcode 0x" << std::hex
                          << static_cast<int>(packet[kIusbScsiOpcodeIndex]) << std::dec
                          << "); no response sent";
            return;
        }
        const ScsiCdb cdb = ScsiCdTarget::parse_cdb(packet + kIusbScsiOpcodeIndex);
        const ScsiResult result = scsi_.execute(cdb);

        // One line per command under --vverbose; a CHECK CONDITION is always logged so a failed
        // read is visible at any verbosity.
        if (options_.login.vverbose || result.status != 0) {
            LogLine line = log_info();
            line << "cd scsi #" << scsi_seen_ << " opcode=0x" << std::hex
                 << static_cast<int>(cdb.opcode) << std::dec << " lba=" << cdb.lba
                 << " len=" << cdb.length << " -> status=" << static_cast<int>(result.status)
                 << " bytes=" << result.data.size();
            if (result.status != 0) {
                line << " sense=" << static_cast<int>(result.sense_key) << '/'
                     << static_cast<int>(result.asc) << '/' << static_cast<int>(result.ascq);
            }
        }
        ++scsi_seen_;
        queue_packet(iusb_build_scsi_response(packet, size, result));

        // Guest-initiated eject: START_STOP_UNIT carrying the eject magic in the CDB's LBA field
        // (little-endian, the IUSB control convention). The guest removed our disc; its response is
        // already queued, so tear the session down gracefully (request_stop publishes Idle on exit,
        // which the title-bar control reflects).
        if (cdb.opcode == kScsiStartStopUnit) {
            const std::uint8_t* lba = packet + kIusbScsiOpcodeIndex + 2;
            const std::uint32_t control_lba = static_cast<std::uint32_t>(lba[0]) |
                (static_cast<std::uint32_t>(lba[1]) << 8) |
                (static_cast<std::uint32_t>(lba[2]) << 16) |
                (static_cast<std::uint32_t>(lba[3]) << 24);
            if (control_lba == kIusbLbaEjected) {
                log_info() << "cd-server: guest ejected the virtual CD";
                request_stop();
            }
        }
    }

    void queue_packet(std::vector<std::uint8_t> bytes)
    {
        if (closed_) {
            return;
        }
        outgoing_.push_back(OutgoingPacket{std::move(bytes), {}});
        start_write();
    }

    void start_write()
    {
        if (closed_ || writing_ || outgoing_.empty()) {
            return;
        }
        writing_ = true;
        OutgoingPacket& packet = outgoing_.front();
        auto self = shared_from_this();
        if (bmc_ws_is_binary_mode(subprotocol_)) {
            ws_->binary(true);
            ws_->async_write(
                asio::buffer(packet.bytes),
                asio::bind_executor(strand_, [self](beast::error_code error, std::size_t) {
                    self->on_write(error);
                }));
            return;
        }
        packet.encoded_text = bmc_base64_encode(packet.bytes);
        ws_->text(true);
        ws_->async_write(
            asio::buffer(packet.encoded_text),
            asio::bind_executor(strand_, [self](beast::error_code error, std::size_t) {
                self->on_write(error);
            }));
    }

    void on_write(beast::error_code error)
    {
        if (error) {
            if (stopping_ || closed_ || error == asio::error::operation_aborted ||
                error == asio::error::bad_descriptor) {
                close_socket();
                return;
            }
            media_.publish_outcome(MediaOutcome{false, std::string("cd-server write failed: ") + error.message()});
            media_.publish_state(MediaState::Error);
            log_error() << "cd-server write error: " << error.message();
            close_socket();
            return;
        }

        if (!outgoing_.empty()) {
            outgoing_.pop_front();
        }
        writing_ = false;

        if (stopping_ && outgoing_.empty()) {
            close_socket();
            return;
        }
        start_write();
    }

    void arm_force_close_timer()
    {
        auto self = shared_from_this();
        force_close_timer_.expires_after(kMediaStopGrace);
        force_close_timer_.async_wait(
            asio::bind_executor(strand_, [self](beast::error_code error) {
                if (error == asio::error::operation_aborted || self->closed_) {
                    return;
                }
                self->close_socket();  // the graceful DISCONNECT didn't drain in time
            }));
    }

    void close_socket()
    {
        if (closed_) {
            return;
        }
        closed_ = true;
        force_close_timer_.cancel();
        beast::error_code error;
        beast::get_lowest_layer(*ws_).socket().shutdown(tcp::socket::shutdown_both, error);
        error.clear();
        beast::get_lowest_layer(*ws_).socket().close(error);
        io_.stop();
    }

    asio::io_context& io_;
    asio::strand<asio::io_context::executor_type> strand_;
    asio::steady_timer force_close_timer_;
    std::shared_ptr<BmcWebSocketStream> ws_;
    MegaracViewOptions options_;
    std::string token_;
    std::string subprotocol_;
    std::string filename_;
    std::uint8_t cd_device_no_;
    BlockSource& source_;
    MediaChannel& media_;
    ScsiCdTarget scsi_;
    beast::flat_buffer read_buffer_;
    std::vector<std::uint8_t> accumulator_;
    std::deque<OutgoingPacket> outgoing_;
    int scsi_seen_ = 0;
    bool ack_seen_ = false;
    bool writing_ = false;
    bool stopping_ = false;
    bool closed_ = false;
};

} // namespace

void run_megarac_media_session(
    const MegaracViewOptions& options,
    const std::string& iso_path,
    MediaSessionState& state,
    const std::atomic_bool& stop_requested)
{
    try {
        // Open the image first: a bad path/format fails fast, before any network work.
        std::unique_ptr<BlockSource> source;
        try {
            source = std::make_unique<IsoFileSource>(iso_path);
        } catch (const std::exception& exception) {
            state.media.publish_outcome(MediaOutcome{false, exception.what()});
            state.media.publish_state(MediaState::Error);
            log_error() << "cannot mount ISO: " << exception.what();
            return;
        }
        // Startup marker: confirms the 64-bit FILE* IsoFileSource is the code that's running and
        // reports the capacity the host will see.
        log_info() << "cd image opened: " << iso_path << " capacity_sectors="
                   << source->capacity_sectors() << " ("
                   << source->capacity_sectors() * source->sector_size() << " bytes)";
        state.media.publish_state(MediaState::Mounting);

        // Login + token fetch are HTTP with no socket registered anywhere yet; an
        // early unmount/exit can only abort them through the cancel token. The slot
        // is replaced with the websocket force-close below once /cd-server opens.
        LoginOptions login_options = options.login;
        if (!login_options.cancel_token) {
            login_options.cancel_token = std::make_shared<HttpCancelToken>();
        }
        state.set_force_close([token = login_options.cancel_token] { token->cancel(); });

        MegaRacSession session = login_megarac(login_options);
        MegaRacLogoutGuard logout_guard(login_options);
        logout_guard.arm(session);
        log_info() << "megarac-cd login succeeded";

        const std::string token = fetch_cd_token(session.web, options.login.verbose);
        session.web.set_cookie("__Host-isActiveKVM", "true");

        // Abort the socket if a stop arrives before the async session is live to handle it.
        state.set_force_close([&web = session.web] { web.force_close_websocket("megarac-cd"); });
        if (stop_requested.load()) {
            state.set_force_close({});
            return;
        }

        auto opened = session.web.open_websocket(BmcWebSocketConnectOptions{
            .role = "megarac-cd",
            .log_name = "cd-server websocket",
            .path = "/cd-server",
            .idle_timeout_seconds = 0,  // a mounted CD legitimately idles between reads
            .extra_headers = {Header{http::field::sec_websocket_protocol, {}, "binary, base64"}},
        });
        auto ws = opened.connection->stream();
        asio::io_context& io = opened.connection->io_context();
        const std::string subprotocol = bmc_ws_selected_subprotocol(opened.response);
        log_info() << "cd-server websocket subprotocol=" << subprotocol;

        auto async_session = std::make_shared<CdServerAsyncSession>(
            io, ws, options, token, subprotocol, filename_from_path(iso_path),
            /*cd_device_no=*/0, *source, state.media);
        // Stop (GUI eject / Ctrl-C) = graceful: flush any in-flight response, send DISCONNECT(247),
        // then close. request_stop() posts onto the strand (woken even when idle) and arms a 250ms
        // grace timer that force-closes if the DISCONNECT write stalls, so io.run() always returns.
        std::weak_ptr<CdServerAsyncSession> weak_session = async_session;
        state.set_force_close([weak_session] {
            if (auto session = weak_session.lock()) {
                session->request_stop();
            }
        });

        async_session->start();
        io.run();

        state.set_force_close({});
        if (state.media.state() != MediaState::Error) {
            state.media.publish_state(MediaState::Idle);
        }
    } catch (const std::exception& exception) {
        state.set_force_close({});
        state.media.publish_outcome(MediaOutcome{false, std::string("media session error: ") + exception.what()});
        state.media.publish_state(MediaState::Error);
        print_current_exception_with_stack(std::cerr, "megarac-cd network thread");
    } catch (...) {
        state.set_force_close({});
        state.media.publish_state(MediaState::Error);
        print_current_exception_with_stack(std::cerr, "megarac-cd network thread");
    }
}

#ifdef _WIN32
namespace {
// Ctrl-C in the CLI test -> stop the session. File-static pointers bridge from the console
// control handler (a separate OS thread Windows spawns) to the running session.
std::atomic<MediaSessionState*> g_cli_media_state{nullptr};
std::atomic_bool* g_cli_stop = nullptr;

// A real named BOOL WINAPI(DWORD) handler -- matches clipp's working pattern. (Run hitsc via
// the hitsc.com shim, not hitsc.exe directly: the shim shields the parent and shares the
// console with this GUI-subsystem child so the event reaches us here.)
BOOL WINAPI cli_console_ctrl_handler(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT) {
        return FALSE;
    }
    if (g_cli_stop != nullptr) {
        g_cli_stop->store(true);
    }
    if (MediaSessionState* media_state = g_cli_media_state.load()) {
        if (auto force_close = media_state->force_close_snapshot()) {
            force_close();  // io.stop() -> io.run() returns -> process exits
        }
    }
    return TRUE;
}
} // namespace
#endif

void run_megarac_media(const MegaracViewOptions& options, const std::string& iso_path)
{
    MediaSessionState state;
    std::atomic_bool stop{false};

#ifdef _WIN32
    // hitsc.exe is /SUBSYSTEM:windows, launched by the hitsc.com shim which forwards our stdio as
    // pipes but does NOT attach us to the console -- and a GUI-subsystem child is not auto-attached.
    // Console control events (Ctrl-C) are only delivered to processes ATTACHED to the console, so
    // SetConsoleCtrlHandler does nothing until we attach to the launching terminal. (Output is
    // unaffected: stdout/stderr remain the shim's pipes; this only joins the console's control
    // group. This is exactly what clipp's InitializeConsoleOutput does via AttachConsole.)
    AttachConsole(ATTACH_PARENT_PROCESS);
    g_cli_media_state.store(&state);
    g_cli_stop = &stop;
    SetConsoleCtrlHandler(cli_console_ctrl_handler, TRUE);
    log_info() << "mounting " << iso_path << " -- press Ctrl-C to unmount";
#endif

    run_megarac_media_session(options, iso_path, state, stop);

#ifdef _WIN32
    SetConsoleCtrlHandler(cli_console_ctrl_handler, FALSE);  // unregister our handler
    g_cli_media_state.store(nullptr);
    g_cli_stop = nullptr;
#endif

    for (const MediaOutcome& outcome : state.media.drain_outcomes()) {
        if (outcome.ok) {
            log_info() << "media: " << outcome.detail;
        } else {
            log_error() << "media: " << outcome.detail;
        }
    }
}

} // namespace hitsc
