#include "megarac_view_session.hpp"

#include "aspeed_decoder.hpp"
#include "diagnostics.hpp"
#include "http_client.hpp"
#include "log.hpp"
#include "megarac_protocol.hpp"
#include "megarac_session.hpp"
#include "megarac_video.hpp"
#include "text.hpp"

#include <SDL3/SDL.h>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace hitsc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace json = boost::json;
namespace ssl = asio::ssl;
namespace websocket = boost::beast::websocket;
using tcp = asio::ip::tcp;
using KvmWebSocket = BmcWebSocketStream;

constexpr auto kBlankRecoveryInterval = std::chrono::seconds(3);
constexpr auto kMegaracStopGrace = std::chrono::milliseconds(250);
constexpr std::size_t kMaxQueuedInputPackets = 128;

constexpr std::uint16_t kCmdSendHidPacket = command_value(MegaracCommand::SendHidPacket);
constexpr std::uint16_t kCmdConnectionAllowed = command_value(MegaracCommand::ConnectionAllowed);
constexpr std::uint16_t kCmdVideoPackets = command_value(MegaracCommand::VideoPackets);
constexpr std::uint16_t kCmdActiveClients = command_value(MegaracCommand::ActiveClients);
constexpr std::uint16_t kCmdKeepAlive = command_value(MegaracCommand::KeepAlive);
constexpr std::uint16_t kCmdValidateVideoSession = command_value(MegaracCommand::ValidateVideoSession);
constexpr std::uint16_t kCmdValidatedVideoSession = command_value(MegaracCommand::ValidatedVideoSession);
constexpr std::uint16_t kCmdMaxSessionClose = command_value(MegaracCommand::MaxSessionClose);
constexpr std::uint16_t kCmdStopSessionImmediate = command_value(MegaracCommand::StopSessionImmediate);
constexpr std::uint16_t kCmdPaintBlankScreen = command_value(MegaracCommand::PaintBlankScreen);
constexpr std::uint16_t kCmdGetFullScreen = command_value(MegaracCommand::GetFullScreen);
constexpr std::uint16_t kCmdUsbMouseMode = command_value(MegaracCommand::UsbMouseMode);
constexpr std::uint16_t kCmdGetWebToken = command_value(MegaracCommand::GetWebToken);
constexpr std::uint16_t kCmdKvmSharing = command_value(MegaracCommand::KvmSharing);
constexpr std::uint16_t kCmdGetUserMacro = command_value(MegaracCommand::GetUserMacro);
constexpr std::uint16_t kCmdSetNextMaster = command_value(MegaracCommand::SetNextMaster);
constexpr std::uint16_t kCmdPowerStatus = command_value(MegaracCommand::PowerStatus);
constexpr std::uint16_t kCmdDisplayLockSet = command_value(MegaracCommand::DisplayLockSet);
constexpr std::uint16_t kCmdMediaLicenseStatus = command_value(MegaracCommand::MediaLicenseStatus);
constexpr std::uint16_t kCmdFpsDiff = command_value(MegaracCommand::FpsDiff);
constexpr std::uint16_t kIvtpHwCursor = command_value(MegaracCommand::IvtpHwCursor);

using KvmConfig = MegaracViewConfig;
using KvmPacket = MegaracPacket;
using PacketBuffer = MegaracPacketBuffer;
using SharedCursor = MegaracHardwareCursor;

constexpr std::uint8_t kValidateSessionValid = kMegaracValidateSessionValid;
constexpr std::uint16_t kKvmPrivReqMaster = kMegaracViewPrivReqMaster;
constexpr std::uint16_t kKvmReqAllowed = kMegaracViewReqAllowed;

struct OutgoingPacket {
    std::uint16_t type = 0;
    std::vector<std::uint8_t> bytes;
    std::string encoded_text;
};

std::string json_string_field(const json::object& object, std::string_view name)
{
    const json::value* value = object.if_contains(name);
    if (value == nullptr) {
        return {};
    }

    if (const json::string* string = value->if_string()) {
        return std::string(*string);
    }
    if (const auto* number = value->if_int64()) {
        return std::to_string(*number);
    }
    if (const auto* number = value->if_uint64()) {
        return std::to_string(*number);
    }
    return {};
}

MegaracViewConfig parse_kvm_config(std::string_view body)
{
    boost::system::error_code error;
    json::value value = json::parse(body, error);
    if (error) {
        throw std::runtime_error("/api/settings/media/h5viewercfg returned invalid JSON: " + error.message());
    }

    const json::object* object = value.if_object();
    if (object == nullptr) {
        throw std::runtime_error("/api/settings/media/h5viewercfg did not return a JSON object");
    }

    MegaracViewConfig config;
    config.client_ip = json_string_field(*object, "client_ip");
    config.session = json_string_field(*object, "session");
    config.token = json_string_field(*object, "token");
    config.server_ip = json_string_field(*object, "server_ip");
    if (config.token.empty()) {
        throw std::runtime_error("/api/settings/media/h5viewercfg did not include a KVM token");
    }
    if (config.client_ip.empty()) {
        config.client_ip = "No IP";
    }
    if (config.server_ip.empty()) {
        config.server_ip = "No IP";
    }
    return config;
}

bool parse_reconnect_feature(std::string_view body)
{
    boost::system::error_code error;
    json::value value = json::parse(body, error);
    if (error) {
        throw std::runtime_error("/api/configuration/project returned invalid JSON: " + error.message());
    }

    const json::array* array = value.if_array();
    if (array == nullptr) {
        return false;
    }

    for (const json::value& item : *array) {
        const json::object* object = item.if_object();
        if (object == nullptr) {
            continue;
        }
        if (json_string_field(*object, "feature") == "KVM_SESSION_RECONNECT") {
            return true;
        }
    }
    return false;
}

bool fetch_reconnect_feature(
    BmcWebSession& web)
{
    std::vector<Header> headers;
    const std::string_view csrf_token = web.session_token();
    if (!csrf_token.empty()) {
        headers.push_back(Header{http::field::unknown, "X-CSRFTOKEN", std::string(csrf_token)});
    }

    auto response = web.request(
        http::verb::get,
        "/api/configuration/project",
        {},
        {},
        headers);

    require_success_status(response, "/api/configuration/project");
    return parse_reconnect_feature(decode_response_body(response));
}

MegaracViewConfig fetch_kvm_config(BmcWebSession& web)
{
    std::vector<Header> headers;
    const std::string_view csrf_token = web.session_token();
    if (!csrf_token.empty()) {
        headers.push_back(Header{http::field::unknown, "X-CSRFTOKEN", std::string(csrf_token)});
    }

    auto response = web.request(
        http::verb::get,
        "/api/settings/media/h5viewercfg",
        {},
        {},
        headers);

    require_success_status(response, "/api/settings/media/h5viewercfg");
    MegaracViewConfig config = parse_kvm_config(decode_response_body(response));
    config.reconnect_enabled = fetch_reconnect_feature(web);
    return config;
}

std::vector<std::uint8_t> base64_decode(std::string_view input)
{
    auto value_of = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') {
            return ch - 'A';
        }
        if (ch >= 'a' && ch <= 'z') {
            return ch - 'a' + 26;
        }
        if (ch >= '0' && ch <= '9') {
            return ch - '0' + 52;
        }
        if (ch == '+') {
            return 62;
        }
        if (ch == '/') {
            return 63;
        }
        return -1;
    };

    std::vector<std::uint8_t> decoded;
    int accumulator = 0;
    int bits = -8;
    for (const char ch : input) {
        if (ch == '=') {
            break;
        }
        const int value = value_of(ch);
        if (value < 0) {
            continue;
        }
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 0) {
            decoded.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xff));
            bits -= 8;
        }
    }
    return decoded;
}

std::string base64_encode(const std::vector<std::uint8_t>& input)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const std::uint32_t a = input[i];
        const std::uint32_t b = i + 1 < input.size() ? input[i + 1] : 0;
        const std::uint32_t c = i + 2 < input.size() ? input[i + 2] : 0;
        const std::uint32_t triple = (a << 16) | (b << 8) | c;

        encoded.push_back(alphabet[(triple >> 18) & 0x3f]);
        encoded.push_back(alphabet[(triple >> 12) & 0x3f]);
        encoded.push_back(i + 1 < input.size() ? alphabet[(triple >> 6) & 0x3f] : '=');
        encoded.push_back(i + 2 < input.size() ? alphabet[triple & 0x3f] : '=');
    }
    return encoded;
}

bool is_binary_mode(std::string_view subprotocol)
{
    return subprotocol == "binary";
}

std::vector<std::uint8_t> message_bytes_from_buffer(
    const beast::flat_buffer& buffer,
    std::string_view subprotocol)
{
    const auto data = buffer.data();
    if (is_binary_mode(subprotocol)) {
        std::vector<std::uint8_t> bytes(boost::asio::buffer_size(data));
        boost::asio::buffer_copy(boost::asio::buffer(bytes), data);
        return bytes;
    }

    std::string text(boost::asio::buffer_size(data), '\0');
    boost::asio::buffer_copy(boost::asio::buffer(text), data);
    return base64_decode(text);
}

std::string selected_subprotocol(const websocket::response_type& response)
{
    const auto field = response.find(http::field::sec_websocket_protocol);
    if (field == response.end()) {
        return "base64";
    }

    std::string protocol = trim_copy(field->value());
    return protocol.empty() ? "base64" : protocol;
}

void publish_frame(MegaracViewSessionState& state, MegaracCompressedFrame frame)
{
    frame.published_at = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(state.frame_mutex);
        frame.sequence = ++state.frame_sequence;
        state.frame = std::make_shared<MegaracCompressedFrame>(std::move(frame));
    }

    const auto frame_event_type = static_cast<Uint32>(state.frame_event_type.load());
    if (frame_event_type != 0 && !state.frame_event_pending.exchange(true)) {
        SDL_Event event{};
        event.type = frame_event_type;
        if (!SDL_PushEvent(&event)) {
            state.frame_event_pending.store(false);
        }
    }
}

void publish_cursor(MegaracViewSessionState& state, SharedCursor cursor)
{
    std::lock_guard lock(state.frame_mutex);
    cursor.sequence = ++state.cursor_sequence;
    state.cursor = std::move(cursor);
    state.has_cursor = true;
}

void set_mouse_mode(MegaracViewSessionState& state, int mouse_mode)
{
    state.mouse_mode.store(mouse_mode);
}

void set_megarac_force_close(MegaracViewSessionState& state, std::function<void()> force_close)
{
    std::lock_guard lock(state.control_mutex);
    state.force_close = std::move(force_close);
}

void clear_megarac_input_sink(MegaracViewSessionState& state)
{
    std::lock_guard lock(state.control_mutex);
    state.input_sink = {};
    state.pending_input.clear();
}

void install_megarac_input_sink(
    MegaracViewSessionState& state,
    std::function<void(std::uint16_t, std::vector<std::uint8_t>)> input_sink)
{
    std::deque<PendingMegaracInputPacket> pending;
    {
        std::lock_guard lock(state.control_mutex);
        state.input_sink = input_sink;
        pending.swap(state.pending_input);
    }

    for (PendingMegaracInputPacket& packet : pending) {
        input_sink(packet.type, std::move(packet.packet));
    }
}

void clear_megarac_control_callbacks(MegaracViewSessionState& state)
{
    std::lock_guard lock(state.control_mutex);
    state.input_sink = {};
    state.pending_input.clear();
    state.force_close = {};
}

void force_close_network(MegaracViewSessionState& state)
{
    std::function<void()> force_close;
    {
        std::lock_guard lock(state.control_mutex);
        force_close = state.force_close;
        state.input_sink = {};
        state.pending_input.clear();
        state.force_close = {};
    }

    if (force_close) {
        force_close();
    }
}

void set_megarac_exception(MegaracViewSessionState& state, std::exception_ptr exception)
{
    std::lock_guard lock(state.control_mutex);
    if (!state.exception) {
        state.exception = exception;
    }
}

std::exception_ptr take_megarac_exception(MegaracViewSessionState& state)
{
    std::lock_guard lock(state.control_mutex);
    return state.exception;
}

int megarac_view_mouse_mode_snapshot(MegaracViewSessionState& state)
{
    return state.mouse_mode.load();
}

bool queue_megarac_view_packet(
    MegaracViewSessionState& state,
    std::uint16_t type,
    std::vector<std::uint8_t> packet)
{
    std::function<void(std::uint16_t, std::vector<std::uint8_t>)> input_sink;
    {
        std::lock_guard lock(state.control_mutex);
        input_sink = state.input_sink;
        if (!input_sink) {
            if (state.pending_input.size() >= kMaxQueuedInputPackets) {
                state.pending_input.pop_front();
            }
            state.pending_input.push_back(PendingMegaracInputPacket{type, std::move(packet)});
            return true;
        }
    }

    input_sink(type, std::move(packet));
    return true;
}

std::shared_ptr<const MegaracCompressedFrame> take_latest_megarac_view_frame(
    MegaracViewSessionState& state,
    std::uint64_t last_sequence)
{
    std::lock_guard lock(state.frame_mutex);
    if (!state.frame || state.frame->sequence == last_sequence) {
        return {};
    }
    return state.frame;
}

void clear_latest_megarac_view_frame(MegaracViewSessionState& state)
{
    std::lock_guard lock(state.frame_mutex);
    state.frame.reset();
}

std::optional<MegaracHardwareCursor> take_latest_megarac_view_cursor(
    MegaracViewSessionState& state,
    std::uint64_t last_sequence)
{
    std::lock_guard lock(state.frame_mutex);
    if (!state.has_cursor || state.cursor.sequence == last_sequence) {
        return std::nullopt;
    }
    return state.cursor;
}

void log_packet(int number, const KvmPacket& packet)
{
    LogLine line = log_info();
    line << "kvm packet #" << number
         << " type=" << packet.type
         << " " << command_name(packet.type)
         << " status=" << packet.status
         << " payload=" << packet.payload_size;

    if (packet.type == kCmdValidatedVideoSession && !packet.payload.empty()) {
        line << " validation=" << static_cast<int>(packet.payload[0]);
    } else if (packet.type == kCmdVideoPackets && packet.payload.size() >= 4) {
        line << " first-bytes=";
        const auto preview = std::min<std::size_t>(packet.payload.size(), 8);
        for (std::size_t i = 0; i < preview; ++i) {
            if (i != 0) {
                line << ' ';
            }
            line << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<int>(packet.payload[i])
                 << std::dec << std::setfill(' ');
        }
    }
}

void log_sent_packet(std::uint16_t type, std::uint16_t status, std::size_t payload_size, bool enabled)
{
    if (!enabled) {
        return;
    }

    log_info() << "sent kvm packet"
               << " type=" << type
               << " " << command_name(type)
               << " status=" << status
               << " payload=" << payload_size;
}

class KvmAsyncSession : public std::enable_shared_from_this<KvmAsyncSession> {
public:
    KvmAsyncSession(
        asio::io_context& io,
        std::shared_ptr<KvmWebSocket> ws,
        MegaracViewOptions options,
        KvmConfig config,
        std::string subprotocol,
        MegaracViewSessionState& state)
        : io_(io)
        , strand_(asio::make_strand(io))
        , force_close_timer_(io)
        , ws_(std::move(ws))
        , options_(std::move(options))
        , config_(std::move(config))
        , subprotocol_(std::move(subprotocol))
        , state_(state)
    {
    }

    void start()
    {
        auto self = shared_from_this();
        asio::dispatch(strand_, [self] {
            self->start_read();
        });
    }

    void send_packet(std::uint16_t type, std::vector<std::uint8_t> packet)
    {
        auto self = shared_from_this();
        asio::post(strand_, [self, type, packet = std::move(packet)]() mutable {
            self->queue_packet_from_strand(type, std::move(packet));
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

            if (self->writing_ && !self->outgoing_packets_.empty()) {
                auto keep_current = self->outgoing_packets_.begin();
                ++keep_current;
                self->outgoing_packets_.erase(keep_current, self->outgoing_packets_.end());
            } else {
                self->outgoing_packets_.clear();
            }

            const bool stop_already_pending = std::any_of(
                self->outgoing_packets_.begin(),
                self->outgoing_packets_.end(),
                [](const OutgoingPacket& packet) {
                    return packet.type == kCmdStopSessionImmediate;
                });
            if (!stop_already_pending) {
                self->outgoing_packets_.push_back(OutgoingPacket{
                    kCmdStopSessionImmediate,
                    make_simple_packet(kCmdStopSessionImmediate),
                    {}});
            }
            self->start_write();
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
            asio::bind_executor(strand_, [self](beast::error_code error, std::size_t bytes_transferred) {
                self->on_read(error, bytes_transferred);
            }));
    }

    void on_read(beast::error_code error, std::size_t bytes_transferred)
    {
        if (error) {
            handle_read_error(error);
            return;
        }

        try {
            state_.view_status.data_received(bytes_transferred);
            last_websocket_message_bytes_ = bytes_transferred;
            packet_buffer_.append(message_bytes_from_buffer(read_buffer_, subprotocol_));
            while (std::optional<KvmPacket> packet = packet_buffer_.next()) {
                handle_packet(*packet);
                if (closed_) {
                    return;
                }
            }
        } catch (...) {
            set_megarac_exception(state_, std::current_exception());
            print_current_exception_with_stack(std::cerr, "kvm websocket handler");
            close_socket();
            return;
        }

        start_read();
    }

    void handle_read_error(beast::error_code error)
    {
        if (stopping_ || closed_ ||
            error == boost::asio::error::operation_aborted ||
            error == boost::asio::error::bad_descriptor) {
            close_socket();
            return;
        }

        if (error == beast::error::timeout) {
            log_warning() << "megarac view idle timeout after "
                          << options_.idle_timeout_seconds << "s";
            close_socket();
            return;
        }

        if (error == websocket::error::closed ||
            error == ssl::error::stream_truncated) {
            log_warning() << "kvm websocket closed";
            close_socket();
            return;
        }

        set_megarac_exception(state_, std::make_exception_ptr(
            beast::system_error(error, "MegaRAC websocket read failed")));
        log_error() << "megarac view error: " << error.message()
                    << " [" << error.category().name() << ':' << error.value() << ']';
        close_socket();
    }

    void handle_packet(const KvmPacket& packet)
    {
        ++packets_seen_;
        if (options_.login.vverbose) {
            log_packet(packets_seen_, packet);
        }

        if (packet.type == kCmdConnectionAllowed && !validation_sent_) {
            queue_packet_from_strand(
                kCmdValidateVideoSession,
                make_validate_video_session_packet(config_, options_.login.username));
            validation_sent_ = true;
            log_info() << "sent KVM validation packet";
        } else if (packet.type == kCmdValidatedVideoSession) {
            handle_validation_response(packet);
        } else if (packet.type == kCmdKeepAlive) {
            queue_packet_from_strand(kCmdKeepAlive, make_simple_packet(kCmdKeepAlive));
        } else if (packet.type == kCmdUsbMouseMode && !packet.payload.empty()) {
            set_mouse_mode(state_, packet.payload[0]);
            if (options_.login.verbose) {
                log_info() << "mouse mode=" << static_cast<int>(packet.payload[0]);
            }
        } else if (packet.type == kCmdMediaLicenseStatus) {
            queue_packet_from_strand(
                kCmdDisplayLockSet,
                make_payload_packet(kCmdDisplayLockSet, 0, std::vector<std::uint8_t>{2}));
            queue_packet_from_strand(kCmdGetUserMacro, make_simple_packet(kCmdGetUserMacro));

            if (!config_.session.empty()) {
                queue_packet_from_strand(kCmdGetWebToken, make_web_token_packet(config_.session));
            }
        } else if (packet.type == kCmdActiveClients && !full_screen_requested_) {
            queue_packet_from_strand(kCmdGetFullScreen, make_simple_packet(kCmdGetFullScreen, 1));
            full_screen_requested_ = true;
            log_info() << "requested full screen";
        } else if (packet.type == kCmdKvmSharing) {
            handle_kvm_sharing(packet);
        } else if (packet.type == kCmdSetNextMaster) {
            query_power_status("next-master");
        } else if (packet.type == kCmdMaxSessionClose) {
            const std::string reason = max_session_close_reason(packet.status);
            set_megarac_exception(state_, std::make_exception_ptr(
                std::runtime_error("MegaRAC KVM session closed: " + reason)));
            log_warning() << "kvm session closed: " << reason
                          << " status=" << packet.status;
            close_socket();
        } else if (packet.type == kCmdPaintBlankScreen) {
            handle_blank_screen_packet(packet);
        } else if (packet.type == kCmdPowerStatus) {
            handle_power_status(packet.status);
        } else if (packet.type == kIvtpHwCursor) {
            handle_hardware_cursor_packet(packet);
        } else if (packet.type == kCmdVideoPackets) {
            handle_video_packet(packet);
        }

        send_fps_report_if_due();
    }

    void handle_validation_response(const KvmPacket& packet)
    {
        if (packet.payload.empty()) {
            set_megarac_exception(state_, std::make_exception_ptr(
                std::runtime_error("MegaRAC KVM validation failed: empty response")));
            log_error() << "KVM validation failed: empty response";
            close_socket();
            return;
        }

        const auto response = static_cast<std::uint8_t>(packet.payload[0]);
        if (response != kValidateSessionValid) {
            const std::string reason = validation_response_name(response);
            set_megarac_exception(state_, std::make_exception_ptr(
                std::runtime_error("MegaRAC KVM validation failed: " + reason)));
            log_error() << "KVM validation failed: " << reason
                        << " response=" << static_cast<int>(response);
            close_socket();
            return;
        }

        state_.view_status.kvm_display_status(true);
    }

    void handle_kvm_sharing(const KvmPacket& packet)
    {
        if ((packet.status & 0xff) != kKvmPrivReqMaster) {
            return;
        }

        const auto allowed_status = static_cast<std::uint16_t>(
            kKvmPrivReqMaster + (kKvmReqAllowed << 8));

        // TODO: prompt before granting KVM sharing once the UI has policy controls.
        queue_packet_from_strand(
            kCmdKvmSharing,
            make_payload_packet(kCmdKvmSharing, allowed_status, packet.payload));
        queue_packet_from_strand(
            kCmdSetNextMaster,
            make_payload_packet(kCmdSetNextMaster, allowed_status, packet.payload));

        log_info() << "granted KVM sharing request"
                   << " payload=" << packet.payload.size();
    }

    void query_power_status(std::string_view reason)
    {
        queue_packet_from_strand(kCmdPowerStatus, make_simple_packet(kCmdPowerStatus));
        if (options_.login.verbose) {
            log_info() << "requested power status"
                       << " reason=" << reason;
        }
    }

    void request_full_screen_retry(std::string_view reason)
    {
        queue_packet_from_strand(kCmdGetFullScreen, make_simple_packet(kCmdGetFullScreen, 1));
        if (options_.login.verbose) {
            log_info() << "requested full screen"
                       << " reason=" << reason;
        }
    }

    void handle_blank_screen_packet(const KvmPacket& packet)
    {
        ++blank_screen_packets_;
        state_.view_status.kvm_display_status(false);
        if (blank_screen_packets_ == 1) {
            log_warning() << "remote requested blank screen"
                          << " status=" << packet.status;
        }

        const auto now = std::chrono::steady_clock::now();
        if (last_blank_recovery_ != std::chrono::steady_clock::time_point{} &&
            now - last_blank_recovery_ < kBlankRecoveryInterval) {
            return;
        }

        last_blank_recovery_ = now;
        query_power_status("blank-screen");
        request_full_screen_retry("blank-screen");
    }

    void handle_power_status(std::uint16_t status)
    {
        power_status_ = static_cast<int>(status);
        if (options_.login.verbose) {
            log_info() << "power status=" << power_status_;
        }
        if (blank_screen_packets_ > 0 && status == 1) {
            request_full_screen_retry("power-on-after-blank");
        }
    }

    void handle_hardware_cursor_packet(const KvmPacket& packet)
    {
        std::optional<SharedCursor> cursor = parse_hardware_cursor_packet(
            packet.payload,
            cursor_pattern_,
            framebuffer_width_,
            framebuffer_height_);
        if (!cursor) {
            return;
        }

        if (cursor->pattern_from_packet) {
            cursor_pattern_ = cursor->pattern;
        }
        const SharedCursor cursor_for_log = *cursor;
        publish_cursor(state_, std::move(*cursor));

        if (options_.login.vverbose) {
            log_info() << "hardware cursor"
                       << " type=" << cursor_for_log.type
                       << " x=" << cursor_for_log.x
                       << " y=" << cursor_for_log.y
                       << " offset=" << cursor_for_log.x_offset << ',' << cursor_for_log.y_offset
                       << " size=" << cursor_for_log.width << 'x' << cursor_for_log.height
                       << " pattern=" << (cursor_for_log.has_pattern ? "yes" : "no")
                       << " checksum=0x" << std::hex << cursor_for_log.checksum << std::dec;
        }
    }

    void handle_video_packet(const KvmPacket& packet)
    {
        ++video_packets_seen_;
        std::optional<MegaracVideoFrame> frame = video_assembler_.ingest(packet.payload);
        if (!frame) {
            if (options_.login.vverbose && video_packets_seen_ <= 20) {
                log_info() << "video packet #" << video_packets_seen_
                           << " payload=" << packet.payload.size()
                           << " complete=no";
            }
            return;
        }

        const std::uint8_t block_header = megarac_video_first_block_header(frame->compressed);
        if (frame->rc4_enable != 0 || !megarac_video_is_supported_first_block(block_header)) {
            log_warning() << "skipped unsupported video frame"
                          << " compression=" << static_cast<int>(frame->compression_mode)
                          << " rc4=" << static_cast<int>(frame->rc4_enable)
                          << " first-block=0x" << std::hex << static_cast<int>(block_header) << std::dec;
            ++frames_received_since_report_;
            fps_reporting_started_ = true;
            return;
        }

        ++frames_received_since_report_;
        fps_reporting_started_ = true;
        blank_screen_packets_ = 0;
        const int next_frame_number = frames_seen_ + 1;
        if (options_.login.vverbose && (next_frame_number <= 20 || next_frame_number % 60 == 0)) {
            log_info() << "queued MegaRAC ASPEED frame #" << next_frame_number
                       << " packets=" << packets_seen_
                       << " video-packets=" << video_packets_seen_
                       << " size=" << frame->width << "x" << frame->height
                       << " compressed=" << frame->compressed.size()
                       << " compression=" << static_cast<int>(frame->compression_mode)
                       << " mode420=" << static_cast<int>(frame->mode420)
                       << " first-block=0x" << std::hex << static_cast<int>(block_header) << std::dec;
        }

        MegaracCompressedFrame compressed_frame;
        compressed_frame.width = frame->width;
        compressed_frame.height = frame->height;
        compressed_frame.frame_number = next_frame_number;
        compressed_frame.compression_mode = frame->compression_mode;
        compressed_frame.first_block_header = block_header;
        compressed_frame.decode_options = make_megarac_aspeed_decode_options(*frame);
        compressed_frame.compressed = std::move(frame->compressed);
        compressed_frame.received_at = std::chrono::steady_clock::now();
        compressed_frame.websocket_bytes = last_websocket_message_bytes_;

        framebuffer_width_ = frame->width;
        framebuffer_height_ = frame->height;
        state_.view_status.kvm_display_status(true);
        publish_frame(state_, std::move(compressed_frame));
        ++frames_seen_;
    }

    void send_fps_report_if_due()
    {
        const auto now = std::chrono::steady_clock::now();
        if (!fps_reporting_started_ || now - last_fps_report_ < std::chrono::milliseconds(100)) {
            return;
        }

        const std::uint64_t frames_presented = state_.frames_presented.load();
        const int frames_presented_since_report =
            static_cast<int>(frames_presented - last_frames_presented_for_report_);
        const int diff = std::abs(frames_received_since_report_ - frames_presented_since_report);
        queue_packet_from_strand(kCmdFpsDiff, make_simple_packet(kCmdFpsDiff, static_cast<std::uint16_t>(diff)));
        frames_received_since_report_ = 0;
        last_frames_presented_for_report_ = frames_presented;
        last_fps_report_ = now;
    }

    void queue_packet_from_strand(std::uint16_t type, std::vector<std::uint8_t> packet)
    {
        if (closed_ || stopping_) {
            return;
        }

        auto mutable_begin = outgoing_packets_.begin();
        if (writing_ && mutable_begin != outgoing_packets_.end()) {
            ++mutable_begin;
        }

        constexpr std::size_t max_queued_packets = 128;
        while (outgoing_packets_.size() >= max_queued_packets) {
            auto removable = std::find_if(
                mutable_begin,
                outgoing_packets_.end(),
                [](const OutgoingPacket& outgoing) {
                    return outgoing.type == kCmdSendHidPacket;
                });
            if (removable == outgoing_packets_.end()) {
                break;
            }
            outgoing_packets_.erase(removable);
            mutable_begin = outgoing_packets_.begin();
            if (writing_ && mutable_begin != outgoing_packets_.end()) {
                ++mutable_begin;
            }
        }

        outgoing_packets_.push_back(OutgoingPacket{type, std::move(packet), {}});
        start_write();
    }

    void discard_queued_packets_preserving_active_write()
    {
        if (writing_ && !outgoing_packets_.empty()) {
            auto keep_current = outgoing_packets_.begin();
            ++keep_current;
            outgoing_packets_.erase(keep_current, outgoing_packets_.end());
            return;
        }

        outgoing_packets_.clear();
    }

    void start_write()
    {
        if (closed_ || writing_ || outgoing_packets_.empty()) {
            return;
        }

        writing_ = true;
        OutgoingPacket& packet = outgoing_packets_.front();
        auto self = shared_from_this();
        if (is_binary_mode(subprotocol_)) {
            ws_->binary(true);
            ws_->async_write(
                asio::buffer(packet.bytes),
                asio::bind_executor(strand_, [self](beast::error_code error, std::size_t) {
                    self->on_write(error);
                }));
            return;
        }

        packet.encoded_text = base64_encode(packet.bytes);
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
            if (stopping_ || closed_ ||
                error == boost::asio::error::operation_aborted ||
                error == boost::asio::error::bad_descriptor) {
                close_socket();
                return;
            }

            set_megarac_exception(state_, std::make_exception_ptr(
                beast::system_error(error, "MegaRAC websocket write failed")));
            log_error() << "kvm write error: " << error.message()
                        << " [" << error.category().name() << ':' << error.value() << ']';
            close_socket();
            return;
        }

        if (!outgoing_packets_.empty()) {
            const OutgoingPacket sent = std::move(outgoing_packets_.front());
            outgoing_packets_.pop_front();
            if (options_.login.vverbose) {
                log_sent_packet(
                    sent.type,
                    packet_status_from_bytes(sent.bytes),
                    packet_payload_size_from_bytes(sent.bytes),
                    true);
            } else if (sent.type == kCmdStopSessionImmediate) {
                log_info() << "sent stop session packet";
            }
        }

        writing_ = false;
        if (stopping_ && outgoing_packets_.empty()) {
            close_socket();
            return;
        }

        start_write();
    }

    void arm_force_close_timer()
    {
        auto self = shared_from_this();
        force_close_timer_.expires_after(kMegaracStopGrace);
        force_close_timer_.async_wait(
            asio::bind_executor(strand_, [self](beast::error_code error) {
                if (error == boost::asio::error::operation_aborted || self->closed_) {
                    return;
                }
                if (error) {
                    log_warning() << "megarac stop timer error=" << error.message();
                    self->close_socket();
                    return;
                }
                if (self->stopping_) {
                    log_warning() << "forcing MegaRAC websocket closed after stop grace";
                    self->close_socket();
                }
            }));
    }

    void close_socket()
    {
        if (closed_) {
            return;
        }

        closed_ = true;
        force_close_timer_.cancel();
        clear_megarac_input_sink(state_);
        set_megarac_force_close(state_, {});
        discard_queued_packets_preserving_active_write();
        beast::error_code error;
        beast::get_lowest_layer(*ws_).socket().shutdown(tcp::socket::shutdown_both, error);
        error.clear();
        beast::get_lowest_layer(*ws_).socket().close(error);
        io_.stop();
    }

    asio::io_context& io_;
    asio::strand<asio::io_context::executor_type> strand_;
    asio::steady_timer force_close_timer_;
    std::shared_ptr<KvmWebSocket> ws_;
    MegaracViewOptions options_;
    KvmConfig config_;
    std::string subprotocol_;
    MegaracViewSessionState& state_;
    beast::flat_buffer read_buffer_;
    PacketBuffer packet_buffer_;
    MegaracVideoAssembler video_assembler_;
    std::vector<std::uint16_t> cursor_pattern_;
    std::deque<OutgoingPacket> outgoing_packets_;
    int framebuffer_width_ = 0;
    int framebuffer_height_ = 0;
    int packets_seen_ = 0;
    int video_packets_seen_ = 0;
    int frames_seen_ = 0;
    int blank_screen_packets_ = 0;
    int power_status_ = -1;
    bool validation_sent_ = false;
    bool full_screen_requested_ = false;
    bool fps_reporting_started_ = false;
    bool writing_ = false;
    bool stopping_ = false;
    bool closed_ = false;
    int frames_received_since_report_ = 0;
    std::uint64_t last_frames_presented_for_report_ = 0;
    std::size_t last_websocket_message_bytes_ = 0;
    std::chrono::steady_clock::time_point last_fps_report_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_blank_recovery_;
};

void stop_megarac_network(
    MegaracViewSessionState& state,
    std::atomic_bool& stop_requested,
    std::thread& network_thread,
    bool verbose)
{
    const auto started_at = std::chrono::steady_clock::now();
    if (verbose) {
        log_debug() << "megarac network stop begin";
    }
    stop_requested.store(true);

    std::function<void()> force_close;
    {
        std::lock_guard lock(state.control_mutex);
        force_close = state.force_close;
    }
    if (force_close) {
        force_close();
    } else if (verbose) {
        log_debug() << "megarac network force-close missing";
    }

    if (network_thread.joinable()) {
        network_thread.join();
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at).count();
    if (elapsed_ms > 250) {
        log_warning() << "megarac network stop waited"
                      << " total-ms=" << elapsed_ms;
    } else if (verbose) {
        log_debug() << "megarac network stop end";
    }
}

void run_megarac_view_session(const MegaracViewOptions& options, MegaracViewSessionState& state, const std::atomic_bool& stop_requested)
{
    struct ConnectionStatusGuard {
        MegaracViewSessionState& state;
        ~ConnectionStatusGuard()
        {
            state.view_status.kvm_connection(false);
        }
    } connection_status_guard{state};

    try {
        MegaRacSession session = login_megarac(options.login);
        MegaRacLogoutGuard logout_guard(options.login);
        logout_guard.arm(session);
        log_info() << "megarac login succeeded";

        KvmConfig config = fetch_kvm_config(session.web);
        session.web.set_cookie("__Host-isActiveKVM", "true");
        log_info() << "kvm config fetched"
                   << " client_ip=" << config.client_ip
                   << " server_ip=" << config.server_ip
                   << " token=present"
                   << " reconnect=" << (config.reconnect_enabled ? "yes" : "no");

        set_megarac_force_close(state, [&web = session.web] {
            web.force_close_websocket("megarac-kvm");
        });

        auto websocket = session.web.open_websocket(BmcWebSocketConnectOptions{
            .role = "megarac-kvm",
            .log_name = "kvm websocket",
            .path = "/kvm",
            .idle_timeout_seconds = options.idle_timeout_seconds,
            .extra_headers = {Header{http::field::sec_websocket_protocol, {}, "binary, base64"}},
        });
        auto ws = websocket.connection->stream();
        asio::io_context& io = websocket.connection->io_context();
        const std::string subprotocol = selected_subprotocol(websocket.response);
        state.view_status.kvm_connection(true);
        log_info() << "kvm websocket subprotocol=" << subprotocol;

        if (stop_requested.load()) {
            force_close_network(state);
            return;
        }

        auto async_session = std::make_shared<KvmAsyncSession>(io, ws, options, config, subprotocol, state);
        std::weak_ptr<KvmAsyncSession> weak_session = async_session;
        install_megarac_input_sink(
            state,
            [weak_session](std::uint16_t type, std::vector<std::uint8_t> packet) mutable {
                if (auto session = weak_session.lock()) {
                    session->send_packet(type, std::move(packet));
                }
            });
        set_megarac_force_close(state, [weak_session] {
            if (auto session = weak_session.lock()) {
                session->request_stop();
            }
        });
        async_session->start();
        io.run();
    } catch (...) {
        if (!stop_requested.load()) {
            set_megarac_exception(state, std::current_exception());
            print_current_exception_with_stack(std::cerr, "kvm network thread");
        }
    }

    clear_megarac_control_callbacks(state);
}

} // namespace hitsc
