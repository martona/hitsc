#include "megarac_view_session.hpp"

#include "app_info.hpp"
#include "aspeed_decoder.hpp"
#include "diagnostics.hpp"
#include "http_client.hpp"
#include "log.hpp"
#include "megarac_hid.hpp"
#include "megarac_protocol.hpp"
#include "megarac_session.hpp"
#include "megarac_video.hpp"
#include "text.hpp"
#include "tls.hpp"
#include "url.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json.hpp>
#include <boost/version.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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
using KvmWebSocket = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

constexpr int kInitialFramebufferWidth = 800;
constexpr int kInitialFramebufferHeight = 600;
constexpr auto kBlankRecoveryInterval = std::chrono::seconds(3);

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

constexpr int kRelativeMouseMode = kMegaracRelativeMouseMode;
constexpr int kOtherMouseMode = kMegaracOtherMouseMode;
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

void set_megarac_view_status(MegaracViewSessionState& state, std::string status)
{
    std::lock_guard lock(state.control_mutex);
    state.status = std::move(status);
}

void publish_frame(MegaracViewSessionState& state, int width, int height, std::vector<std::uint8_t> rgba)
{
    std::lock_guard lock(state.frame_mutex);
    state.frame.width = width;
    state.frame.height = height;
    state.frame.rgba = std::move(rgba);
    ++state.frame.sequence;
    state.has_frame = true;
}

void publish_cursor(MegaracViewSessionState& state, SharedCursor cursor)
{
    std::lock_guard lock(state.cursor_mutex);
    cursor.sequence = state.cursor.sequence + 1;
    state.cursor = std::move(cursor);
    state.has_cursor = true;
}

void set_subprotocol(MegaracViewSessionState& state, std::string subprotocol)
{
    std::lock_guard lock(state.control_mutex);
    state.subprotocol = std::move(subprotocol);
}

void set_mouse_mode(MegaracViewSessionState& state, int mouse_mode)
{
    std::lock_guard lock(state.control_mutex);
    state.mouse_mode = mouse_mode;
}

int megarac_view_mouse_mode_snapshot(MegaracViewSessionState& state)
{
    std::lock_guard lock(state.control_mutex);
    return state.mouse_mode;
}

bool queue_megarac_view_packet(
    MegaracViewSessionState& state,
    std::uint16_t type,
    std::vector<std::uint8_t> packet,
    bool coalesce)
{
    std::function<void(std::uint16_t, std::vector<std::uint8_t>, bool)> send_packet;
    {
        std::lock_guard lock(state.control_mutex);
        send_packet = state.send_packet;
    }

    if (send_packet) {
        send_packet(type, std::move(packet), coalesce);
        return true;
    }
    return false;
}

std::optional<MegaracViewFrame> take_latest_megarac_view_frame(MegaracViewSessionState& state, std::uint64_t last_sequence)
{
    std::lock_guard lock(state.frame_mutex);
    if (!state.has_frame || state.frame.sequence == last_sequence) {
        return std::nullopt;
    }
    return state.frame;
}

std::optional<MegaracHardwareCursor> take_latest_megarac_view_cursor(MegaracViewSessionState& state, std::uint64_t last_sequence)
{
    std::lock_guard lock(state.cursor_mutex);
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

void store_force_close_callback(MegaracViewSessionState& state, std::function<void()> force_close)
{
    std::lock_guard lock(state.control_mutex);
    state.force_close = std::move(force_close);
}

void clear_connection_callbacks(MegaracViewSessionState& state)
{
    std::lock_guard lock(state.control_mutex);
    state.subprotocol.clear();
    state.force_close = {};
}

void store_network_callbacks(
    MegaracViewSessionState& state,
    std::function<void(std::uint16_t, std::vector<std::uint8_t>, bool)> send_packet,
    std::function<void()> stop_network)
{
    std::lock_guard lock(state.control_mutex);
    state.send_packet = std::move(send_packet);
    state.stop_network = std::move(stop_network);
}

void clear_network_callbacks(MegaracViewSessionState& state)
{
    std::lock_guard lock(state.control_mutex);
    state.send_packet = {};
    state.stop_network = {};
}

void stop_megarac_view_session(MegaracViewSessionState& state)
{
    std::function<void()> stop_network;
    std::function<void()> force_close;
    {
        std::lock_guard lock(state.control_mutex);
        stop_network = state.stop_network;
        force_close = state.force_close;
    }

    if (stop_network) {
        stop_network();
        return;
    }

    if (force_close) {
        force_close();
    }
}

void force_close_network(MegaracViewSessionState& state)
{
    std::function<void()> force_close;
    {
        std::lock_guard lock(state.control_mutex);
        force_close = state.force_close;
        state.subprotocol.clear();
        state.send_packet = {};
        state.stop_network = {};
        state.force_close = {};
    }

    if (force_close) {
        force_close();
    }
}

MegaracViewStatusSnapshot megarac_view_status_snapshot(MegaracViewSessionState& state)
{
    MegaracViewStatusSnapshot snapshot;
    {
        std::lock_guard lock(state.control_mutex);
        snapshot.status = state.status;
    }
    {
        std::lock_guard lock(state.frame_mutex);
        snapshot.has_frame = state.has_frame;
    }
    return snapshot;
}

int sampled_average_rgb(const std::vector<std::uint8_t>& rgba)
{
    if (rgba.empty()) {
        return 0;
    }

    std::uint64_t sum = 0;
    std::uint64_t samples = 0;
    const std::size_t pixels = rgba.size() / 4;
    const std::size_t stride = std::max<std::size_t>(1, pixels / 4096);
    for (std::size_t pixel = 0; pixel < pixels; pixel += stride) {
        const std::size_t offset = pixel * 4;
        sum += rgba[offset] + rgba[offset + 1] + rgba[offset + 2];
        ++samples;
    }
    return samples == 0 ? 0 : static_cast<int>(sum / (samples * 3));
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
        : strand_(asio::make_strand(io))
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

    void send_packet(std::uint16_t type, std::vector<std::uint8_t> packet, bool coalesce)
    {
        auto self = shared_from_this();
        asio::post(strand_, [self, type, packet = std::move(packet), coalesce]() mutable {
            self->queue_packet_from_strand(type, std::move(packet), coalesce);
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
            set_megarac_view_status(self->state_, "stopping");

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
            asio::bind_executor(strand_, [self](beast::error_code error, std::size_t) {
                self->on_read(error);
            }));
    }

    void on_read(beast::error_code error)
    {
        if (error) {
            handle_read_error(error);
            return;
        }

        try {
            packet_buffer_.append(message_bytes_from_buffer(read_buffer_, subprotocol_));
            while (std::optional<KvmPacket> packet = packet_buffer_.next()) {
                handle_packet(*packet);
                if (closed_) {
                    return;
                }
            }
        } catch (const std::exception& ex) {
            fail_with_exception(ex);
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
            set_megarac_view_status(state_, "idle timeout");
            log_warning() << "megarac view idle timeout after "
                          << options_.idle_timeout_seconds << "s";
            close_socket();
            return;
        }

        if (error == websocket::error::closed ||
            error == ssl::error::stream_truncated) {
            set_megarac_view_status(state_, "remote closed");
            log_warning() << "kvm websocket closed";
            close_socket();
            return;
        }

        set_megarac_view_status(state_, std::string("error: ") + error.message());
        log_error() << "megarac view error: " << error.message()
                    << " [" << error.category().name() << ':' << error.value() << ']';
        close_socket();
    }

    void handle_packet(const KvmPacket& packet)
    {
        ++packets_seen_;
        if (options_.login.verbose) {
            log_packet(packets_seen_, packet);
        }

        if (packet.type == kCmdConnectionAllowed && !validation_sent_) {
            queue_packet_from_strand(
                kCmdValidateVideoSession,
                make_validate_video_session_packet(config_, options_.login.username),
                false);
            validation_sent_ = true;
            log_info() << "sent KVM validation packet";
        } else if (packet.type == kCmdValidatedVideoSession) {
            handle_validation_response(packet);
        } else if (packet.type == kCmdKeepAlive) {
            queue_packet_from_strand(kCmdKeepAlive, make_simple_packet(kCmdKeepAlive), false);
        } else if (packet.type == kCmdUsbMouseMode && !packet.payload.empty()) {
            set_mouse_mode(state_, packet.payload[0]);
            mouse_mode_seen_ = true;
            if (options_.login.verbose) {
                log_info() << "mouse mode=" << static_cast<int>(packet.payload[0]);
            }
            send_initial_wake_hid_if_ready();
        } else if (packet.type == kCmdMediaLicenseStatus) {
            queue_packet_from_strand(
                kCmdDisplayLockSet,
                make_payload_packet(kCmdDisplayLockSet, 0, std::vector<std::uint8_t>{2}),
                false);
            queue_packet_from_strand(kCmdGetUserMacro, make_simple_packet(kCmdGetUserMacro), false);

            if (!config_.session.empty()) {
                queue_packet_from_strand(kCmdGetWebToken, make_web_token_packet(config_.session), false);
            }
        } else if (packet.type == kCmdActiveClients && !full_screen_requested_) {
            queue_packet_from_strand(kCmdGetFullScreen, make_simple_packet(kCmdGetFullScreen, 1), false);
            full_screen_requested_ = true;
            log_info() << "requested full screen";
        } else if (packet.type == kCmdKvmSharing) {
            handle_kvm_sharing(packet);
        } else if (packet.type == kCmdSetNextMaster) {
            query_power_status("next-master");
        } else if (packet.type == kCmdMaxSessionClose) {
            const std::string reason = max_session_close_reason(packet.status);
            set_megarac_view_status(state_, "session closed: " + reason);
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
            validation_failed_ = true;
            set_megarac_view_status(state_, "validation failed: empty response");
            log_error() << "KVM validation failed: empty response";
            close_socket();
            return;
        }

        const auto response = static_cast<std::uint8_t>(packet.payload[0]);
        if (response != kValidateSessionValid) {
            validation_failed_ = true;
            const std::string reason = validation_response_name(response);
            set_megarac_view_status(state_, "validation failed: " + reason);
            log_error() << "KVM validation failed: " << reason
                        << " response=" << static_cast<int>(response);
            close_socket();
            return;
        }

        session_validated_ = true;
        set_megarac_view_status(state_, "validated");
        send_initial_wake_hid_if_ready();
    }

    void send_initial_wake_hid_if_ready()
    {
        if (!session_validated_ || !mouse_mode_seen_ || initial_wake_hid_sent_) {
            return;
        }

        initial_wake_hid_sent_ = true;
        if (deferred_hid_packet_) {
            outgoing_packets_.push_back(std::move(*deferred_hid_packet_));
            deferred_hid_packet_.reset();
            start_write();
            if (options_.login.verbose) {
                log_info() << "released deferred HID packet after validation";
            }
            return;
        }

        const int mouse_mode = megarac_view_mouse_mode_snapshot(state_);
        std::vector<std::uint8_t> packet;
        if (mouse_mode == kRelativeMouseMode || mouse_mode == kOtherMouseMode) {
            packet = make_megarac_relative_mouse_packet(
                MegaracRelativeMouseReport{0, 1, 1, 0},
                synthetic_mouse_sequence_++);
        } else {
            packet = make_megarac_absolute_mouse_packet(
                MegaracAbsoluteMouseReport{
                    0,
                    kInitialFramebufferWidth / 2,
                    kInitialFramebufferHeight / 2,
                    kInitialFramebufferWidth,
                    kInitialFramebufferHeight,
                    0},
                synthetic_mouse_sequence_++);
        }

        queue_packet_from_strand(kCmdSendHidPacket, std::move(packet), false);
        if (options_.login.verbose) {
            log_info() << "sent synthetic wake mouse"
                       << " mode=" << mouse_mode
                       << " x=" << (kInitialFramebufferWidth / 2)
                       << " y=" << (kInitialFramebufferHeight / 2);
        }
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
            make_payload_packet(kCmdKvmSharing, allowed_status, packet.payload),
            false);
        queue_packet_from_strand(
            kCmdSetNextMaster,
            make_payload_packet(kCmdSetNextMaster, allowed_status, packet.payload),
            false);

        log_info() << "granted KVM sharing request"
                   << " payload=" << packet.payload.size();
    }

    void query_power_status(std::string_view reason)
    {
        queue_packet_from_strand(kCmdPowerStatus, make_simple_packet(kCmdPowerStatus), false);
        if (options_.login.verbose) {
            log_info() << "requested power status"
                       << " reason=" << reason;
        }
    }

    void request_full_screen_retry(std::string_view reason)
    {
        queue_packet_from_strand(kCmdGetFullScreen, make_simple_packet(kCmdGetFullScreen, 1), false);
        if (options_.login.verbose) {
            log_info() << "requested full screen"
                       << " reason=" << reason;
        }
    }

    void handle_blank_screen_packet(const KvmPacket& packet)
    {
        ++blank_screen_packets_;
        set_megarac_view_status(state_, "blank screen");
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

        if (options_.login.verbose) {
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
        const std::optional<MegaracVideoFrame> frame = video_assembler_.ingest(packet.payload);
        if (!frame) {
            if (options_.login.verbose && video_packets_seen_ <= 20) {
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
        AspeedDecodeOptions decode_options;
        decode_options.width = frame->width;
        decode_options.height = frame->height;
        decode_options.mode420 = frame->mode420;
        decode_options.jpeg_table_selector = frame->jpeg_table_selector;
        decode_options.advance_table_selector = frame->advance_table_selector;
        const int next_frame_number = frames_seen_ + 1;
        if (options_.login.verbose && (next_frame_number <= 20 || next_frame_number % 60 == 0)) {
            log_info() << "decoding frame #" << next_frame_number
                       << " packets=" << packets_seen_
                       << " video-packets=" << video_packets_seen_
                       << " size=" << frame->width << "x" << frame->height
                       << " compressed=" << frame->compressed.size()
                       << " compression=" << static_cast<int>(frame->compression_mode)
                       << " mode420=" << static_cast<int>(frame->mode420)
                       << " first-block=0x" << std::hex << static_cast<int>(block_header) << std::dec;
        }
        const bool can_use_previous =
            framebuffer_width_ == frame->width
            && framebuffer_height_ == frame->height
            && framebuffer_.size()
                == static_cast<std::size_t>(frame->width) * static_cast<std::size_t>(frame->height) * 4;

        std::vector<std::uint8_t> rgba =
            decoder_.decode_rgba(decode_options, frame->compressed, can_use_previous ? &framebuffer_ : nullptr);
        const int average_rgb = sampled_average_rgb(rgba);
        framebuffer_ = rgba;
        framebuffer_width_ = frame->width;
        framebuffer_height_ = frame->height;
        publish_frame(state_, frame->width, frame->height, std::move(rgba));
        ++frames_processed_since_report_;

        ++frames_seen_;
        if (options_.login.verbose && (frames_seen_ <= 20 || frames_seen_ % 60 == 0)) {
            log_info() << "rendered frame #" << frames_seen_
                       << " packets=" << packets_seen_
                       << " video-packets=" << video_packets_seen_
                       << " size=" << frame->width << "x" << frame->height
                       << " compressed=" << frame->compressed.size()
                       << " avg-rgb=" << average_rgb;
        }
    }

    void send_fps_report_if_due()
    {
        const auto now = std::chrono::steady_clock::now();
        if (!fps_reporting_started_ || now - last_fps_report_ < std::chrono::milliseconds(100)) {
            return;
        }

        const int diff = std::abs(frames_received_since_report_ - frames_processed_since_report_);
        queue_packet_from_strand(kCmdFpsDiff, make_simple_packet(kCmdFpsDiff, static_cast<std::uint16_t>(diff)), false);
        frames_received_since_report_ = 0;
        frames_processed_since_report_ = 0;
        last_fps_report_ = now;
    }

    void queue_packet_from_strand(std::uint16_t type, std::vector<std::uint8_t> packet, bool coalesce)
    {
        if (closed_ || stopping_) {
            return;
        }

        if (type == kCmdSendHidPacket && !session_validated_) {
            if (validation_failed_) {
                return;
            }

            deferred_hid_packet_ = OutgoingPacket{type, std::move(packet), {}};
            if (options_.login.verbose && !deferred_hid_logged_) {
                deferred_hid_logged_ = true;
                log_info() << "deferring HID packets until KVM validation succeeds";
            }
            return;
        }

        auto mutable_begin = outgoing_packets_.begin();
        if (writing_ && mutable_begin != outgoing_packets_.end()) {
            ++mutable_begin;
        }

        if (coalesce) {
            for (auto it = outgoing_packets_.end(); it != mutable_begin;) {
                --it;
                if (it->type == type) {
                    it->bytes = std::move(packet);
                    it->encoded_text.clear();
                    return;
                }
            }
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

            set_megarac_view_status(state_, std::string("error: ") + error.message());
            log_error() << "kvm write error: " << error.message()
                        << " [" << error.category().name() << ':' << error.value() << ']';
            close_socket();
            return;
        }

        if (!outgoing_packets_.empty()) {
            const OutgoingPacket sent = std::move(outgoing_packets_.front());
            outgoing_packets_.pop_front();
            if (options_.login.verbose) {
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

    void fail_with_exception(const std::exception& ex)
    {
        set_megarac_view_status(state_, std::string("error: ") + ex.what());
        print_exception_with_stack(std::cerr, ex, "kvm websocket handler");
        close_socket();
    }

    void close_socket()
    {
        if (closed_) {
            return;
        }

        closed_ = true;
        clear_network_callbacks(state_);
        discard_queued_packets_preserving_active_write();
        beast::error_code error;
        beast::get_lowest_layer(*ws_).socket().shutdown(tcp::socket::shutdown_both, error);
        error.clear();
        beast::get_lowest_layer(*ws_).socket().close(error);
        if (stopping_) {
            set_megarac_view_status(state_, "stopped");
        }
    }

    asio::strand<asio::io_context::executor_type> strand_;
    std::shared_ptr<KvmWebSocket> ws_;
    MegaracViewOptions options_;
    KvmConfig config_;
    std::string subprotocol_;
    MegaracViewSessionState& state_;
    beast::flat_buffer read_buffer_;
    PacketBuffer packet_buffer_;
    AspeedDecoder decoder_;
    MegaracVideoAssembler video_assembler_;
    std::vector<std::uint8_t> framebuffer_;
    std::vector<std::uint16_t> cursor_pattern_;
    std::deque<OutgoingPacket> outgoing_packets_;
    std::optional<OutgoingPacket> deferred_hid_packet_;
    std::uint32_t synthetic_mouse_sequence_ = 0;
    int framebuffer_width_ = 0;
    int framebuffer_height_ = 0;
    int packets_seen_ = 0;
    int video_packets_seen_ = 0;
    int frames_seen_ = 0;
    int blank_screen_packets_ = 0;
    int power_status_ = -1;
    bool validation_sent_ = false;
    bool session_validated_ = false;
    bool validation_failed_ = false;
    bool deferred_hid_logged_ = false;
    bool mouse_mode_seen_ = false;
    bool initial_wake_hid_sent_ = false;
    bool full_screen_requested_ = false;
    bool fps_reporting_started_ = false;
    bool writing_ = false;
    bool stopping_ = false;
    bool closed_ = false;
    int frames_received_since_report_ = 0;
    int frames_processed_since_report_ = 0;
    std::chrono::steady_clock::time_point last_fps_report_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_blank_recovery_;
};

void run_megarac_view_session(const MegaracViewOptions& options, MegaracViewSessionState& state, const std::atomic_bool& stop_requested)
{
    try {
        set_megarac_view_status(state, "logging in");
        MegaRacSession session = login_megarac(options.login);
        MegaRacLogoutGuard logout_guard(options.login);
        logout_guard.arm(session);
        log_info() << "megarac login succeeded";

        set_megarac_view_status(state, "fetching KVM config");
        KvmConfig config = fetch_kvm_config(session.web);
        session.web.set_cookie("__Host-isActiveKVM", "true");
        log_info() << "kvm config fetched"
                   << " client_ip=" << config.client_ip
                   << " server_ip=" << config.server_ip
                   << " token=present"
                   << " reconnect=" << (config.reconnect_enabled ? "yes" : "no");

        asio::io_context io;
        ssl::context tls_context(ssl::context::tls_client);
        auto ws = std::make_shared<KvmWebSocket>(io, tls_context);
        std::weak_ptr<KvmWebSocket> weak_ws = ws;
        store_force_close_callback(state, [weak_ws] {
            const std::shared_ptr<KvmWebSocket> ws = weak_ws.lock();
            if (!ws) {
                return;
            }

            beast::error_code error;
            beast::get_lowest_layer(*ws).socket().cancel(error);
            error.clear();
            beast::get_lowest_layer(*ws).socket().shutdown(tcp::socket::shutdown_both, error);
            error.clear();
            beast::get_lowest_layer(*ws).socket().close(error);
        });
        tcp::resolver resolver(io);

        configure_tls(tls_context, ws->next_layer(), options.login.base_url.host, options.login.insecure);
        set_server_name_indication(ws->next_layer(), options.login.base_url.host);

        set_megarac_view_status(state, "connecting");
        const auto endpoints = resolver.resolve(options.login.base_url.host, options.login.base_url.port);
        beast::get_lowest_layer(*ws).expires_after(std::chrono::seconds(30));
        beast::get_lowest_layer(*ws).connect(endpoints);
        ws->next_layer().handshake(ssl::stream_base::client);
        beast::get_lowest_layer(*ws).expires_never();

        websocket::stream_base::timeout timeout;
        timeout.handshake_timeout = std::chrono::seconds(30);
        if (options.idle_timeout_seconds > 0) {
            timeout.idle_timeout = std::chrono::seconds(options.idle_timeout_seconds);
            timeout.keep_alive_pings = true;
        } else {
            timeout.idle_timeout = websocket::stream_base::none();
            timeout.keep_alive_pings = false;
        }
        ws->set_option(timeout);

        const std::string host = make_host_header(options.login.base_url);
        ws->set_option(websocket::stream_base::decorator([&](websocket::request_type& request) {
            request.set(http::field::user_agent, std::string(kName) + "/" + std::string(BOOST_LIB_VERSION));
            request.set(http::field::origin, make_origin(options.login.base_url));
            request.set(http::field::sec_websocket_protocol, "binary, base64");

            const std::string cookie_header = session.web.cookies().header();
            if (!cookie_header.empty()) {
                request.set(http::field::cookie, cookie_header);
            }
        }));

        websocket::response_type response;
        ws->handshake(response, host, "/kvm");
        const std::string subprotocol = selected_subprotocol(response);
        set_subprotocol(state, subprotocol);
        set_megarac_view_status(state, "connected");
        {
            LogLine line = log_info();
            line << "kvm websocket connected"
                 << " subprotocol=" << subprotocol;
            if (options.idle_timeout_seconds > 0) {
                line << " idle-timeout=" << options.idle_timeout_seconds << "s";
            } else {
                line << " idle-timeout=disabled";
            }
        }

        if (stop_requested.load()) {
            force_close_network(state);
            return;
        }

        auto async_session = std::make_shared<KvmAsyncSession>(io, ws, options, config, subprotocol, state);
        std::weak_ptr<KvmAsyncSession> weak_session = async_session;
        store_network_callbacks(
            state,
            [weak_session](std::uint16_t type, std::vector<std::uint8_t> packet, bool coalesce) mutable {
                if (auto session = weak_session.lock()) {
                    session->send_packet(type, std::move(packet), coalesce);
                }
            },
            [weak_session] {
                if (auto session = weak_session.lock()) {
                    session->request_stop();
                }
            });
        async_session->start();
        io.run();
    } catch (const std::exception& ex) {
        set_megarac_view_status(state, std::string("error: ") + ex.what());
        print_exception_with_stack(std::cerr, ex, "kvm network thread");
    }

    clear_network_callbacks(state);
    clear_connection_callbacks(state);
}

} // namespace hitsc
