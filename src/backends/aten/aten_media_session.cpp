#include "backends/aten/aten_media_session.hpp"

#include "backends/aten/aten_session.hpp"
#include "backends/aten/aten_usb_bot.hpp"
#include "backends/megarac/bmc_ws_framing.hpp"
#include "bmc_session.hpp"
#include "diagnostics.hpp"
#include "http_client.hpp"
#include "log.hpp"
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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hitsc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
namespace websocket = boost::beast::websocket;
using tcp = asio::ip::tcp;

namespace {

constexpr auto kMediaStopGrace = std::chrono::milliseconds(250);
// One CBW read is at most a 64-sector transfer (128 KB) plus framing; anything much larger means
// the framing has desynchronized, so we bail rather than buffer forever.
constexpr std::size_t kMaxBotMessage = 256 * 1024;

std::uint32_t random_nonce()
{
    // The reference uses Math.random()*32768 (a 15-bit nonce); match the range with a real RNG.
    std::random_device device;
    std::uniform_int_distribution<std::uint32_t> distribution(0, 32767);
    return distribution(device);
}

// The async state machine for one /vm connection. Structurally a sibling of the MegaRAC
// CdServerAsyncSession (strand, read loop with reassembly, a write deque, graceful stop) with
// the USB-BOT handshake + CBW/CSW dispatch swapped in. Serves SCSI from a BlockSource through
// the shared ScsiCdTarget (full device-emulation persona); knows nothing about the disk format.
class AtenBotAsyncSession : public std::enable_shared_from_this<AtenBotAsyncSession> {
public:
    AtenBotAsyncSession(
        asio::io_context& io,
        std::shared_ptr<BmcWebSocketStream> ws,
        AtenViewOptions options,
        AtenCredential credential,
        std::string subprotocol,
        std::string filename,
        BlockSource& source,
        MediaChannel& media)
        : io_(io)
        , strand_(asio::make_strand(io))
        , force_close_timer_(io)
        , ws_(std::move(ws))
        , options_(std::move(options))
        , credential_(std::move(credential))
        , subprotocol_(std::move(subprotocol))
        , filename_(std::move(filename))
        , source_(source)
        , media_(media)
        , scsi_(source, ScsiPersona::FullDeviceEmulation)
    {
    }

    void start()
    {
        auto self = shared_from_this();
        asio::dispatch(strand_, [self] {
            self->media_.publish_state(MediaState::Mounting);
            // The reference runs a fresh websocket PER pre-mount stage: connection 1 asks
            // GetDevStatus, the BMC replies then CLOSES it; connection 2 asks GetHttpPort, same;
            // connection 3 sends the plug-in as its FIRST message and stays open for the CBW
            // loop. Those two queries are GUI bookkeeping (device-exists table / HTTP port) a
            // headless mount doesn't need, so we skip straight to the plug-in on this one
            // connection -- exactly what the reference's connection 3 does.
            self->send_plug_in();
            self->stage_ = Stage::PlugIn;
            self->start_read();
        });
    }

    // Graceful eject: send the plug-out and let the read loop see the socket close.
    void request_stop()
    {
        auto self = shared_from_this();
        asio::post(strand_, [self] {
            if (self->closed_ || self->stopping_) {
                return;
            }
            self->stopping_ = true;
            self->queue_packet(aten_build_plug_out());
            self->arm_force_close_timer();
        });
    }

private:
    enum class Stage {
        PlugIn,      // sent plug-in, awaiting the class-2 mount status
        CbwLoop,     // mounted: CBWs, keepalives, unmount
    };

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
            drain_accumulator();
            if (closed_) {
                return;
            }
        } catch (...) {
            media_.publish_outcome(MediaOutcome{false, "media session protocol error"});
            media_.publish_state(MediaState::Error);
            print_current_exception_with_stack(std::cerr, "aten /vm handler");
            close_socket();
            return;
        }

        start_read();
    }

    void drain_accumulator()
    {
        while (!closed_) {
            if (bulk_out_remaining_ > 0) {
                consume_bulk_out();
                continue;
            }
            if (accumulator_.size() < kAtenPduTagSize) {
                return;
            }
            AtenPduTag tag{};
            aten_parse_pdu_tag(accumulator_.data(), accumulator_.size(), tag);
            if (tag.length > kMaxBotMessage) {
                throw std::runtime_error("ATEN PDU length out of range; framing desynced");
            }

            if (stage_ == Stage::PlugIn) {
                if (!handle_plug_in_reply(tag)) {
                    return;  // wait for the rest of the mount-status reply
                }
                continue;
            }

            if (!handle_loop_pdu(tag)) {
                return;  // wait for the rest of the message
            }
        }
    }

    // The plug-in reply is a class-2 mount-status PDU with a fixed 9-byte payload (the reference
    // reads exactly 9 regardless of the tag length field). Returns false until it has arrived.
    bool handle_plug_in_reply(const AtenPduTag& tag)
    {
        constexpr std::size_t kMountStatusPayload = 9;
        const std::size_t total = kAtenPduTagSize + kMountStatusPayload;
        if (accumulator_.size() < total) {
            return false;
        }
        const std::vector<std::uint8_t> payload(
            accumulator_.begin() + kAtenPduTagSize, accumulator_.begin() + total);
        accumulator_.erase(accumulator_.begin(), accumulator_.begin() + total);
        handle_mount_status(tag, payload);
        return true;
    }

    void handle_mount_status(const AtenPduTag& tag, const std::vector<std::uint8_t>& payload)
    {
        if (tag.pdu_class != kAtenClassMountStatus || payload.empty()) {
            media_.publish_outcome(MediaOutcome{false, "unexpected plug-in response from BMC"});
            media_.publish_state(MediaState::Error);
            log_error() << "aten /vm plug-in: unexpected response class=" << tag.pdu_class
                        << " payload=" << payload.size();
            close_socket();
            return;
        }

        const std::uint8_t code = payload[0];
        if (code >= kAtenPlugInAuthFail && code <= kAtenPlugInExpire) {
            const char* reason = plug_in_failure_reason(code);
            media_.publish_outcome(MediaOutcome{false, std::string("Media redirection rejected: ") + reason});
            media_.publish_state(MediaState::Error);
            log_error() << "aten /vm plug-in rejected: " << reason << " (code " << static_cast<int>(code) << ")";
            close_socket();
            return;
        }

        // Accepted: send the endpoint max-packet table, then serve CBWs.
        queue_packet(aten_build_set_ep());
        stage_ = Stage::CbwLoop;
        media_.publish_state(MediaState::Mounted);
        media_.publish_outcome(MediaOutcome{true, "Mounted " + filename_});
        log_info() << "aten /vm redirection accepted; serving virtual CD";
    }

    // Returns false if the full message (tag + body) has not arrived yet.
    bool handle_loop_pdu(const AtenPduTag& tag)
    {
        // A CBW is a PDU whose length is exactly 31; classify control PDUs by class.
        if (tag.length == kAtenCbwSize) {
            const std::size_t total = kAtenPduTagSize + kAtenCbwSize;
            if (accumulator_.size() < total) {
                return false;
            }
            AtenCbw cbw{};
            aten_parse_cbw(accumulator_.data() + kAtenPduTagSize, kAtenCbwSize, cbw);
            const std::uint8_t dev_id = tag.dev_id;
            accumulator_.erase(accumulator_.begin(), accumulator_.begin() + total);
            dispatch_cbw(cbw, dev_id);
            return true;
        }

        // Non-CBW control PDU: consume the 8-byte tag (these carry no separate body here).
        switch (tag.pdu_class) {
        case kAtenClassKeepAliveCmd:
            accumulator_.erase(accumulator_.begin(), accumulator_.begin() + kAtenPduTagSize);
            queue_packet(aten_build_keepalive_reply());
            if (options_.login.verbose) {
                log_info() << "aten /vm keep-alive received; replying";
            }
            return true;
        case kAtenClassUnmountResp:
            accumulator_.erase(accumulator_.begin(), accumulator_.begin() + kAtenPduTagSize);
            log_info() << "aten /vm redirection ended by host (unmount)";
            close_socket();
            return true;
        default:
            // Unknown control PDU: drop the tag and resync on the next.
            log_warning() << "aten /vm: ignoring unknown PDU class=" << tag.pdu_class
                          << " length=" << tag.length;
            accumulator_.erase(accumulator_.begin(), accumulator_.begin() + kAtenPduTagSize);
            return true;
        }
    }

    void dispatch_cbw(const AtenCbw& cbw, std::uint8_t dev_id)
    {
        const ScsiCdb cdb = ScsiCdTarget::parse_cdb(cbw.cb);
        const ScsiResult result = scsi_.execute(cdb);
        const std::uint8_t csw_status = result.status == 0 ? 0u : 1u;

        if (++scsi_seen_ <= 16 || options_.login.vverbose) {
            log_info() << "aten scsi #" << scsi_seen_ << " opcode=0x" << std::hex
                       << static_cast<int>(cdb.opcode) << std::dec << " lba=" << cdb.lba
                       << " len=" << cdb.length << " dir=" << (cbw.data_in() ? "in" : "out")
                       << " -> status=" << static_cast<int>(csw_status)
                       << " bytes=" << result.data.size();
        }

        if (cbw.data_in()) {
            queue_packet(aten_build_data_in_reply(cbw, dev_id, result.data, csw_status));
            maybe_handle_guest_eject(cbw);
            return;
        }

        // Data-OUT (e.g. a stray WRITE / MODE SELECT on a read-only CD) or a zero-length command.
        // For a data-OUT transfer the host sends a following bulk-out data PDU we must drain
        // before answering; stash the CSW and consume it, then reply.
        if (cbw.data_transfer_length > 0) {
            bulk_out_remaining_ = static_cast<std::size_t>(cbw.data_transfer_length);
            pending_csw_ = aten_build_csw_reply(cbw, dev_id, csw_status);
            have_pending_csw_ = true;
            return;
        }
        queue_packet(aten_build_csw_reply(cbw, dev_id, csw_status));
        maybe_handle_guest_eject(cbw);
    }

    // A guest ejecting the disc issues START STOP UNIT with LoEj set and Start clear. Unlike
    // MegaRAC (a magic-LBA IUSB control op), ATEN surfaces it as a normal SCSI command; we
    // acknowledge it above, then gracefully unplug so the title-bar glyph returns to Idle and
    // the ISO can be re-mounted -- matching the MegaRAC eject behavior.
    void maybe_handle_guest_eject(const AtenCbw& cbw)
    {
        if (cbw.cb[0] != kScsiStartStopUnit) {
            return;
        }
        const bool load_eject = (cbw.cb[4] & 0x02) != 0;
        const bool start = (cbw.cb[4] & 0x01) != 0;
        if (load_eject && !start) {
            log_info() << "aten /vm: guest ejected the disc; unplugging";
            request_stop();
        }
    }

    // Discard a data-OUT bulk phase ([8-byte data PDU tag][dtl bytes]) we don't act on, then
    // answer the stashed CSW. Read-only media never writes, so this is a defensive drain.
    void consume_bulk_out()
    {
        if (accumulator_.size() < kAtenPduTagSize) {
            return;  // wait for the bulk-out data PDU tag
        }
        // The bulk-out data arrives as one data PDU: tag + up-to-dtl bytes. Drop the tag once,
        // then the data bytes as they arrive.
        if (!bulk_out_tag_seen_) {
            accumulator_.erase(accumulator_.begin(), accumulator_.begin() + kAtenPduTagSize);
            bulk_out_tag_seen_ = true;
        }
        const std::size_t take = std::min(bulk_out_remaining_, accumulator_.size());
        accumulator_.erase(accumulator_.begin(), accumulator_.begin() + take);
        bulk_out_remaining_ -= take;
        if (bulk_out_remaining_ == 0) {
            bulk_out_tag_seen_ = false;
            if (have_pending_csw_) {
                have_pending_csw_ = false;
                queue_packet(std::move(pending_csw_));
            }
        }
    }

    void send_plug_in()
    {
        AtenPlugInAuth auth;
        auth.username = credential_.username;
        auth.password = credential_.password;
        auth.timestamp = random_nonce();
        auth.host_class = kAtenHostClassCdrom;
        queue_packet(aten_build_plug_in(auth));
        log_info() << "aten /vm plug-in sent (host_class=CDROM)";
    }

    void handle_read_error(beast::error_code error)
    {
        if (stopping_ || closed_ || error == asio::error::operation_aborted ||
            error == asio::error::bad_descriptor) {
            close_socket();
            return;
        }
        if (error == websocket::error::closed || error == ssl::error::stream_truncated) {
            log_warning() << "aten /vm websocket closed";
            close_socket();
            return;
        }
        media_.publish_outcome(MediaOutcome{false, std::string("aten /vm read failed: ") + error.message()});
        media_.publish_state(MediaState::Error);
        log_error() << "aten /vm error: " << error.message()
                    << " [" << error.category().name() << ':' << error.value() << ']';
        close_socket();
    }

    void queue_packet(std::vector<std::uint8_t> bytes)
    {
        if (closed_) {
            return;
        }
        outgoing_.push_back(std::move(bytes));
        start_write();
    }

    void start_write()
    {
        if (closed_ || writing_ || outgoing_.empty()) {
            return;
        }
        writing_ = true;
        auto self = shared_from_this();
        ws_->binary(true);  // ATEN /vm is binary-only (subprotocol "binary")
        ws_->async_write(
            asio::buffer(outgoing_.front()),
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
            media_.publish_outcome(MediaOutcome{false, std::string("aten /vm write failed: ") + error.message()});
            media_.publish_state(MediaState::Error);
            log_error() << "aten /vm write error: " << error.message();
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
                self->close_socket();  // the graceful plug-out didn't drain in time
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

    static const char* plug_in_failure_reason(std::uint8_t code)
    {
        switch (code) {
        case kAtenPlugInAuthFail:  return "authentication failed";
        case kAtenPlugInBusy:      return "system busy";
        case kAtenPlugInPrivilege: return "insufficient privilege";
        case kAtenPlugInDetach:    return "server detached the device";
        case kAtenPlugInFwUpdate:  return "BMC firmware update in progress";
        case kAtenPlugInExpire:    return "session expired";
        default:                   return "unknown error";
        }
    }

    asio::io_context& io_;
    asio::strand<asio::io_context::executor_type> strand_;
    asio::steady_timer force_close_timer_;
    std::shared_ptr<BmcWebSocketStream> ws_;
    AtenViewOptions options_;
    AtenCredential credential_;
    std::string subprotocol_;
    std::string filename_;
    BlockSource& source_;
    MediaChannel& media_;
    ScsiCdTarget scsi_;
    beast::flat_buffer read_buffer_;
    std::vector<std::uint8_t> accumulator_;
    std::deque<std::vector<std::uint8_t>> outgoing_;
    Stage stage_ = Stage::PlugIn;
    int scsi_seen_ = 0;
    std::size_t bulk_out_remaining_ = 0;
    bool bulk_out_tag_seen_ = false;
    std::vector<std::uint8_t> pending_csw_;
    bool have_pending_csw_ = false;
    bool writing_ = false;
    bool stopping_ = false;
    bool closed_ = false;
};

std::string filename_from_path(const std::string& iso_path)
{
    std::error_code ignored;
    std::filesystem::path path(iso_path);
    std::string name = path.filename().string();
    return name.empty() ? "image.iso" : name;
}

} // namespace

void run_aten_media_session(
    const AtenViewOptions& options,
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
        log_info() << "aten cd image opened: " << iso_path << " capacity_sectors="
                   << source->capacity_sectors() << " ("
                   << source->capacity_sectors() * source->sector_size() << " bytes)";
        state.media.publish_state(MediaState::Mounting);

        // Login + bootstrap fetch are HTTP with no socket registered anywhere yet; an early
        // unmount/exit can only abort them through the cancel token, replaced by the websocket
        // force-close once /vm opens.
        LoginOptions login_options = options.login;
        if (!login_options.cancel_token) {
            login_options.cancel_token = std::make_shared<HttpCancelToken>();
        }
        state.set_force_close([token = login_options.cancel_token] { token->cancel(); });

        AtenSession session = login_aten(login_options);
        AtenLogoutGuard logout_guard(login_options);
        logout_guard.arm(session);
        log_info() << "aten-media login succeeded";

        const std::string entry_value = fetch_aten_ikvm_bootstrap(options.login, session.web);
        if (entry_value.empty()) {
            state.set_force_close({});
            state.media.publish_outcome(MediaOutcome{false, "BMC did not provide a virtual-media credential"});
            state.media.publish_state(MediaState::Error);
            log_error() << "aten-media: bootstrap did not expose entry_value";
            return;
        }
        const AtenCredential credential = aten_split_credential(entry_value);

        state.set_force_close([&web = session.web] { web.force_close_websocket("aten-media"); });
        if (stop_requested.load()) {
            state.set_force_close({});
            return;
        }

        auto opened = session.web.open_websocket(BmcWebSocketConnectOptions{
            .role = "aten-media",
            .log_name = "aten /vm websocket",
            .path = "/vm",
            .idle_timeout_seconds = 0,  // a mounted CD legitimately idles between reads
            .tcp_no_delay = true,
            .extra_headers = {Header{http::field::sec_websocket_protocol, {}, "binary"}},
        });
        auto ws = opened.connection->stream();
        asio::io_context& io = opened.connection->io_context();
        const std::string subprotocol = bmc_ws_selected_subprotocol(opened.response);
        log_info() << "aten /vm websocket subprotocol=" << subprotocol;

        auto async_session = std::make_shared<AtenBotAsyncSession>(
            io, ws, options, credential, subprotocol, filename_from_path(iso_path),
            *source, state.media);
        std::weak_ptr<AtenBotAsyncSession> weak_session = async_session;
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
        print_current_exception_with_stack(std::cerr, "aten-media network thread");
    } catch (...) {
        state.set_force_close({});
        state.media.publish_state(MediaState::Error);
        print_current_exception_with_stack(std::cerr, "aten-media network thread");
    }
}

#ifdef _WIN32
namespace {
// Ctrl-C in the CLI test -> stop the session. File-static pointers bridge from the console
// control handler (a separate OS thread Windows spawns) to the running session.
std::atomic<MediaSessionState*> g_cli_media_state{nullptr};
std::atomic_bool* g_cli_stop = nullptr;

BOOL WINAPI aten_cli_console_ctrl_handler(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT) {
        return FALSE;
    }
    if (g_cli_stop != nullptr) {
        g_cli_stop->store(true);
    }
    if (MediaSessionState* media_state = g_cli_media_state.load()) {
        if (auto force_close = media_state->force_close_snapshot()) {
            force_close();
        }
    }
    return TRUE;
}
} // namespace
#endif

void run_aten_media(const AtenViewOptions& options, const std::string& iso_path)
{
    MediaSessionState state;
    std::atomic_bool stop{false};

#ifdef _WIN32
    // See run_megarac_media: hitsc.exe is /SUBSYSTEM:windows launched by the hitsc.com shim, so
    // we must attach to the parent console for SetConsoleCtrlHandler to receive Ctrl-C.
    AttachConsole(ATTACH_PARENT_PROCESS);
    g_cli_media_state.store(&state);
    g_cli_stop = &stop;
    SetConsoleCtrlHandler(aten_cli_console_ctrl_handler, TRUE);
    log_info() << "mounting " << iso_path << " -- press Ctrl-C to unmount";
#endif

    run_aten_media_session(options, iso_path, state, stop);

#ifdef _WIN32
    SetConsoleCtrlHandler(aten_cli_console_ctrl_handler, FALSE);
    g_cli_media_state.store(nullptr);
    g_cli_stop = nullptr;
#endif
}

} // namespace hitsc
