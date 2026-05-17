#include "kvm_view.hpp"

#include "app_info.hpp"
#include "aspeed_decoder.hpp"
#include "http_client.hpp"
#include "kvm_video.hpp"
#include "megarac_session.hpp"
#include "text.hpp"
#include "tls.hpp"
#include "url.hpp"

#include <SDL3/SDL.h>

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
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
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
using KvmWebSocket = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

namespace {

constexpr std::uint16_t kCmdSendHidPacket = 1;
constexpr std::uint16_t kCmdConnectionAllowed = 23;
constexpr std::uint16_t kCmdVideoPackets = 25;
constexpr std::uint16_t kCmdActiveClients = 39;
constexpr std::uint16_t kCmdKeepAlive = 57;
constexpr std::uint16_t kCmdValidateVideoSession = 18;
constexpr std::uint16_t kCmdValidatedVideoSession = 19;
constexpr std::uint16_t kCmdMaxSessionClose = 22;
constexpr std::uint16_t kCmdStopSessionImmediate = 8;
constexpr std::uint16_t kCmdGetFullScreen = 11;
constexpr std::uint16_t kCmdResumeRedirection = 6;
constexpr std::uint16_t kCmdUsbMouseMode = 10;
constexpr std::uint16_t kCmdGetKbdLedStatus = 20;
constexpr std::uint16_t kCmdGetWebToken = 21;
constexpr std::uint16_t kCmdGetUserMacro = 40;
constexpr std::uint16_t kCmdSetNextMaster = 50;
constexpr std::uint16_t kCmdDisplayLockSet = 51;
constexpr std::uint16_t kCmdDisplayControlStatus = 52;
constexpr std::uint16_t kCmdMediaLicenseStatus = 53;
constexpr std::uint16_t kCmdMediaFreeInstanceStatus = 56;
constexpr std::uint16_t kCmdFpsDiff = 60;

constexpr std::size_t kSsiHashSize = 129;
constexpr std::size_t kClientUsernameLength = 129;
constexpr std::size_t kClientOwnIpLength = 65;
constexpr std::size_t kClientOwnMacLength = 49;
constexpr std::size_t kVideoPacketSize = 373;
constexpr std::size_t kWebTokenPayloadLength = 35;
constexpr int kAbsoluteMouseMode = 2;
constexpr int kRelativeMouseMode = 1;
constexpr int kOtherMouseMode = 3;
constexpr int kAbsoluteMouseMax = 32767;
constexpr std::size_t kIusbHeaderSize = 32;
constexpr std::size_t kIusbHidHeaderSize = 34;
constexpr std::size_t kUsbMouseAbsReportSize = 6;
constexpr std::size_t kUsbMouseRelReportSize = 4;
constexpr std::uint8_t kIusbMajor = 1;
constexpr std::uint8_t kIusbMinor = 0;
constexpr std::uint8_t kIusbDeviceMouse = 49;
constexpr std::uint8_t kIusbProtoMouseData = 32;
constexpr std::uint8_t kIusbFromRemote = 128;
constexpr std::uint8_t kIusbMouseDeviceNumber = 2;
constexpr std::uint8_t kIusbMouseInterfaceNumber = 1;
constexpr std::size_t kIusbChecksumOffset = 11;
constexpr std::uint8_t kMouseLeftButton = 1;
constexpr std::uint8_t kMouseRightButton = 2;
constexpr std::uint8_t kMouseMiddleButton = 4;
constexpr std::uint64_t kMouseMotionIntervalMilliseconds = 8;
constexpr int kMaxSdlEventsPerFrame = 96;

struct KvmConfig {
    std::string client_ip;
    std::string session;
    std::string token;
    std::string server_ip;
    bool reconnect_enabled = false;
};

struct KvmPacket {
    std::uint16_t type = 0;
    std::uint32_t payload_size = 0;
    std::uint16_t status = 0;
    std::vector<std::uint8_t> payload;
};

struct SharedFrame {
    int width = 0;
    int height = 0;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> rgba;
};

struct RemoteMousePosition {
    int x = 0;
    int y = 0;
};

struct OutgoingPacket {
    std::uint16_t type = 0;
    std::vector<std::uint8_t> bytes;
    std::string encoded_text;
};

struct ViewState {
    std::mutex frame_mutex;
    std::mutex control_mutex;
    SharedFrame frame;
    std::string status = "starting";
    std::string subprotocol;
    std::function<void(std::uint16_t, std::vector<std::uint8_t>, bool)> send_packet;
    std::function<void()> stop_network;
    std::shared_ptr<KvmWebSocket> websocket;
    bool has_frame = false;
    int mouse_mode = kAbsoluteMouseMode;
};

struct ViewStatusSnapshot {
    std::string status;
    bool has_frame = false;
};

class PacketBuffer {
public:
    void append(const std::vector<std::uint8_t>& bytes)
    {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

    std::optional<KvmPacket> next()
    {
        constexpr std::size_t header_size = 8;
        if (buffer_.size() - offset_ < header_size) {
            compact_if_needed();
            return std::nullopt;
        }

        const auto* data = buffer_.data() + offset_;
        const auto type = read_le16(data);
        const auto payload_size = read_le32(data + 2);
        const auto status = read_le16(data + 6);
        const std::size_t packet_size = header_size + payload_size;
        if (buffer_.size() - offset_ < packet_size) {
            compact_if_needed();
            return std::nullopt;
        }

        KvmPacket packet;
        packet.type = type;
        packet.payload_size = payload_size;
        packet.status = status;
        packet.payload.assign(data + header_size, data + packet_size);
        offset_ += packet_size;
        compact_if_needed();
        return packet;
    }

private:
    static std::uint16_t read_le16(const std::uint8_t* data)
    {
        return static_cast<std::uint16_t>(data[0]) |
               static_cast<std::uint16_t>(data[1] << 8);
    }

    static std::uint32_t read_le32(const std::uint8_t* data)
    {
        return static_cast<std::uint32_t>(data[0]) |
               (static_cast<std::uint32_t>(data[1]) << 8) |
               (static_cast<std::uint32_t>(data[2]) << 16) |
               (static_cast<std::uint32_t>(data[3]) << 24);
    }

    void compact_if_needed()
    {
        if (offset_ == 0) {
            return;
        }
        if (offset_ == buffer_.size()) {
            buffer_.clear();
            offset_ = 0;
            return;
        }
        if (offset_ > 4096) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
            offset_ = 0;
        }
    }

    std::vector<std::uint8_t> buffer_;
    std::size_t offset_ = 0;
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

KvmConfig parse_kvm_config(std::string_view body)
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

    KvmConfig config;
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

bool fetch_reconnect_feature(const LoginOptions& options, CookieJar& cookies, std::string_view csrf_token)
{
    std::vector<Header> headers{
        Header{http::field::origin, {}, make_origin(options.base_url)},
        Header{http::field::referer, {}, make_origin(options.base_url) + "/"},
    };
    if (!csrf_token.empty()) {
        headers.push_back(Header{http::field::unknown, "X-CSRFTOKEN", std::string(csrf_token)});
    }

    auto response = https_request(
        options.base_url,
        options.insecure,
        http::verb::get,
        "/api/configuration/project",
        {},
        {},
        &cookies,
        headers,
        options.verbose);

    require_success_status(response, "/api/configuration/project");
    return parse_reconnect_feature(decode_response_body(response));
}

KvmConfig fetch_kvm_config(const LoginOptions& options, CookieJar& cookies, std::string_view csrf_token)
{
    std::vector<Header> headers{
        Header{http::field::origin, {}, make_origin(options.base_url)},
        Header{http::field::referer, {}, make_origin(options.base_url) + "/"},
    };
    if (!csrf_token.empty()) {
        headers.push_back(Header{http::field::unknown, "X-CSRFTOKEN", std::string(csrf_token)});
    }

    auto response = https_request(
        options.base_url,
        options.insecure,
        http::verb::get,
        "/api/settings/media/h5viewercfg",
        {},
        {},
        &cookies,
        headers,
        options.verbose);

    require_success_status(response, "/api/settings/media/h5viewercfg");
    KvmConfig config = parse_kvm_config(decode_response_body(response));
    config.reconnect_enabled = fetch_reconnect_feature(options, cookies, csrf_token);
    return config;
}

void append_le16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void append_le32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

std::uint8_t to_byte(int value)
{
    return static_cast<std::uint8_t>(value & 0xff);
}

void append_fixed_cstring(std::vector<std::uint8_t>& bytes, std::string_view value, std::size_t fixed_size)
{
    const std::size_t copy_size = std::min(value.size(), fixed_size);
    for (std::size_t i = 0; i < copy_size; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value[i]));
    }
    bytes.insert(bytes.end(), fixed_size - copy_size, 0);
}

std::vector<std::uint8_t> make_header(
    std::uint16_t type,
    std::uint32_t payload_size,
    std::uint16_t status)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(8 + payload_size);
    append_le16(bytes, type);
    append_le32(bytes, payload_size);
    append_le16(bytes, status);
    return bytes;
}

std::vector<std::uint8_t> make_simple_packet(std::uint16_t type, std::uint16_t status = 0)
{
    return make_header(type, 0, status);
}

std::vector<std::uint8_t> make_payload_packet(
    std::uint16_t type,
    std::uint16_t status,
    const std::vector<std::uint8_t>& payload)
{
    std::vector<std::uint8_t> bytes = make_header(
        type,
        static_cast<std::uint32_t>(payload.size()),
        status);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<std::uint8_t> make_web_token_packet(std::string_view session)
{
    std::vector<std::uint8_t> payload;
    payload.reserve(kWebTokenPayloadLength);
    append_fixed_cstring(payload, session, kWebTokenPayloadLength);
    return make_payload_packet(kCmdGetWebToken, 0, payload);
}

void append_iusb_mouse_header(
    std::vector<std::uint8_t>& payload,
    std::size_t report_size,
    std::uint32_t sequence)
{
    constexpr std::string_view signature = "IUSB    ";
    payload.insert(payload.end(), signature.begin(), signature.end());
    payload.push_back(kIusbMajor);
    payload.push_back(kIusbMinor);
    payload.push_back(static_cast<std::uint8_t>(kIusbHeaderSize));
    payload.push_back(0);
    append_le32(
        payload,
        static_cast<std::uint32_t>(kIusbHidHeaderSize - 1 + report_size - kIusbHeaderSize));
    payload.push_back(0);
    payload.push_back(kIusbDeviceMouse);
    payload.push_back(kIusbProtoMouseData);
    payload.push_back(kIusbFromRemote);
    payload.push_back(kIusbMouseDeviceNumber);
    payload.push_back(kIusbMouseInterfaceNumber);
    payload.push_back(0);
    payload.push_back(0);
    append_le32(payload, sequence);
    payload.push_back(0);
    payload.push_back(0);
    payload.push_back(0);
    payload.push_back(0);
}

void set_iusb_checksum(std::vector<std::uint8_t>& payload)
{
    if (payload.size() < kIusbHeaderSize) {
        return;
    }

    payload[kIusbChecksumOffset] = 0;
    int sum = 0;
    for (std::size_t index = 0; index < kIusbHeaderSize; ++index) {
        sum = (sum + payload[index]) & 0xff;
    }
    payload[kIusbChecksumOffset] = to_byte(-sum);
}

std::uint16_t scale_absolute_mouse_coordinate(int value, int size)
{
    if (size <= 0) {
        return 0;
    }

    const int clamped = std::clamp(value, 0, size);
    const double scaled =
        static_cast<double>(clamped) * static_cast<double>(kAbsoluteMouseMax) / static_cast<double>(size);
    return static_cast<std::uint16_t>(std::clamp(
        static_cast<int>(std::floor(scaled + 0.5)),
        0,
        kAbsoluteMouseMax));
}

std::vector<std::uint8_t> make_absolute_mouse_packet(
    std::uint8_t buttons,
    int x,
    int y,
    int width,
    int height,
    int wheel,
    std::uint32_t sequence)
{
    std::vector<std::uint8_t> payload;
    payload.reserve(kIusbHidHeaderSize - 1 + kUsbMouseAbsReportSize);
    append_iusb_mouse_header(payload, kUsbMouseAbsReportSize, sequence);
    payload.push_back(static_cast<std::uint8_t>(kUsbMouseAbsReportSize));
    payload.push_back(buttons);
    append_le16(payload, scale_absolute_mouse_coordinate(x, width));
    append_le16(payload, scale_absolute_mouse_coordinate(y, height));
    payload.push_back(to_byte(wheel));
    set_iusb_checksum(payload);
    return make_payload_packet(kCmdSendHidPacket, 0, payload);
}

std::vector<std::uint8_t> make_relative_mouse_packet(
    std::uint8_t buttons,
    int dx,
    int dy,
    int wheel,
    std::uint32_t sequence)
{
    std::vector<std::uint8_t> payload;
    payload.reserve(kIusbHidHeaderSize - 1 + kUsbMouseRelReportSize);
    append_iusb_mouse_header(payload, kUsbMouseRelReportSize, sequence);
    payload.push_back(static_cast<std::uint8_t>(kUsbMouseAbsReportSize));
    payload.push_back(buttons);
    payload.push_back(to_byte(std::clamp(dx, -127, 127)));
    payload.push_back(to_byte(std::clamp(dy, -127, 127)));
    payload.push_back(to_byte(wheel));
    set_iusb_checksum(payload);
    return make_payload_packet(kCmdSendHidPacket, 0, payload);
}

std::vector<std::uint8_t> make_validate_packet(const KvmConfig& config, std::string_view username)
{
    std::vector<std::uint8_t> payload;
    payload.reserve(kVideoPacketSize + kClientOwnIpLength);
    payload.push_back(0);
    append_fixed_cstring(payload, config.token, kSsiHashSize);
    append_fixed_cstring(payload, config.client_ip, kClientOwnIpLength);
    append_fixed_cstring(payload, username.empty() ? "domain/username" : username, kClientUsernameLength);
    append_fixed_cstring(payload, "00-00-00-00-00-00", kClientOwnMacLength);
    append_fixed_cstring(payload, config.server_ip, kClientOwnIpLength);

    std::vector<std::uint8_t> bytes;
    if (config.reconnect_enabled) {
        bytes = make_simple_packet(58, 1);
    }

    std::vector<std::uint8_t> validate = make_header(
        kCmdValidateVideoSession,
        static_cast<std::uint32_t>(payload.size()),
        1);
    bytes.insert(bytes.end(), validate.begin(), validate.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());

    std::vector<std::uint8_t> resume = make_simple_packet(kCmdResumeRedirection);
    bytes.insert(bytes.end(), resume.begin(), resume.end());
    return bytes;
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

void set_status(ViewState& state, std::string status)
{
    std::lock_guard lock(state.control_mutex);
    state.status = std::move(status);
}

void publish_frame(ViewState& state, int width, int height, std::vector<std::uint8_t> rgba)
{
    std::lock_guard lock(state.frame_mutex);
    state.frame.width = width;
    state.frame.height = height;
    state.frame.rgba = std::move(rgba);
    ++state.frame.sequence;
    state.has_frame = true;
}

void set_subprotocol(ViewState& state, std::string subprotocol)
{
    std::lock_guard lock(state.control_mutex);
    state.subprotocol = std::move(subprotocol);
}

void set_mouse_mode(ViewState& state, int mouse_mode)
{
    std::lock_guard lock(state.control_mutex);
    state.mouse_mode = mouse_mode;
}

int mouse_mode_snapshot(ViewState& state)
{
    std::lock_guard lock(state.control_mutex);
    return state.mouse_mode;
}

bool queue_outgoing_packet(
    ViewState& state,
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

std::optional<SharedFrame> take_latest_frame(ViewState& state, std::uint64_t last_sequence)
{
    std::lock_guard lock(state.frame_mutex);
    if (!state.has_frame || state.frame.sequence == last_sequence) {
        return std::nullopt;
    }
    return state.frame;
}

std::string packet_name(std::uint16_t type)
{
    switch (type) {
    case kCmdSendHidPacket:
        return "CMD_SEND_HID_PACKET";
    case 4:
        return "CMD_PAUSE_REDIRECTION";
    case kCmdResumeRedirection:
        return "CMD_RESUME_REDIRECTION";
    case kCmdStopSessionImmediate:
        return "CMD_STOP_SESSION_IMMEDIATE";
    case kCmdUsbMouseMode:
        return "CMD_USB_MOUSE_MODE";
    case kCmdGetFullScreen:
        return "CMD_GET_FULL_SCREEN";
    case kCmdValidateVideoSession:
        return "CMD_VALIDATE_VIDEO_SESSION";
    case kCmdValidatedVideoSession:
        return "CMD_VALIDATED_VIDEO_SESSION";
    case kCmdMaxSessionClose:
        return "CMD_MAX_SESSION_CLOSE";
    case kCmdGetKbdLedStatus:
        return "CMD_GET_KBD_LED_STATUS";
    case kCmdGetWebToken:
        return "CMD_GET_WEB_TOKEN";
    case kCmdConnectionAllowed:
        return "CMD_CONNECTION_ALLOWED";
    case kCmdVideoPackets:
        return "CMD_VIDEO_PACKETS";
    case 32:
        return "CMD_KVM_SHARING";
    case 34:
        return "CMD_POWER_STATUS";
    case 37:
        return "CMD_SERVICE_INFO";
    case 38:
        return "CMD_KVM_MEDIA_INFO";
    case kCmdActiveClients:
        return "CMD_ACTIVE_CLIENTS";
    case kCmdGetUserMacro:
        return "CMD_GET_USER_MACRO";
    case kCmdSetNextMaster:
        return "CMD_SET_NEXT_MASTER";
    case kCmdDisplayLockSet:
        return "CMD_DISPLAY_LOCK_SET";
    case kCmdDisplayControlStatus:
        return "CMD_DISPLAY_CONTROL_STATUS";
    case kCmdMediaLicenseStatus:
        return "CMD_MEDIA_LICENSE_STATUS";
    case kCmdMediaFreeInstanceStatus:
        return "CMD_MEDIA_FREE_INSTANCE_STATUS";
    case kCmdKeepAlive:
        return "CMD_KEEP_ALIVE_PKT";
    case 58:
        return "CMD_CONNECTION_COMPLETE_PKT";
    case 59:
        return "CMD_CONNECTION_FAILED";
    case kCmdFpsDiff:
        return "CMD_FPS_DIFF";
    case 61:
        return "CMD_KBD_QUEUE_STATUS";
    case 4098:
        return "IVTP_HW_CURSOR";
    case 4099:
        return "IVTP_GET_VIDEO_ENGINE_CONFIGS";
    case 4100:
        return "IVTP_SET_VIDEO_ENGINE_CONFIGS";
    default:
        return "UNKNOWN";
    }
}

void log_packet(int number, const KvmPacket& packet)
{
    std::cout << "hitsc: kvm packet #" << number
              << " type=" << packet.type
              << " " << packet_name(packet.type)
              << " status=" << packet.status
              << " payload=" << packet.payload_size;

    if (packet.type == kCmdValidatedVideoSession && !packet.payload.empty()) {
        std::cout << " validation=" << static_cast<int>(packet.payload[0]);
    } else if (packet.type == kCmdVideoPackets && packet.payload.size() >= 4) {
        std::cout << " first-bytes=";
        const auto preview = std::min<std::size_t>(packet.payload.size(), 8);
        for (std::size_t i = 0; i < preview; ++i) {
            if (i != 0) {
                std::cout << ' ';
            }
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(packet.payload[i])
                      << std::dec << std::setfill(' ');
        }
    }

    std::cout << '\n';
}

void log_sent_packet(std::uint16_t type, std::uint16_t status, std::size_t payload_size, bool enabled)
{
    if (!enabled) {
        return;
    }

    std::cout << "hitsc: sent kvm packet"
              << " type=" << type
              << " " << packet_name(type)
              << " status=" << status
              << " payload=" << payload_size
              << '\n';
}

void store_websocket(ViewState& state, std::shared_ptr<KvmWebSocket> ws)
{
    std::lock_guard lock(state.control_mutex);
    state.websocket = std::move(ws);
    if (!state.websocket) {
        state.subprotocol.clear();
    }
}

void store_network_callbacks(
    ViewState& state,
    std::function<void(std::uint16_t, std::vector<std::uint8_t>, bool)> send_packet,
    std::function<void()> stop_network)
{
    std::lock_guard lock(state.control_mutex);
    state.send_packet = std::move(send_packet);
    state.stop_network = std::move(stop_network);
}

void clear_network_callbacks(ViewState& state)
{
    std::lock_guard lock(state.control_mutex);
    state.send_packet = {};
    state.stop_network = {};
}

void cancel_network(ViewState& state)
{
    std::shared_ptr<KvmWebSocket> ws;
    std::function<void()> stop_network;
    {
        std::lock_guard lock(state.control_mutex);
        ws = state.websocket;
        stop_network = state.stop_network;
    }

    if (stop_network) {
        stop_network();
        return;
    }

    if (!ws) {
        return;
    }

    beast::error_code error;
    beast::get_lowest_layer(*ws).socket().cancel(error);
}

void force_close_network(ViewState& state)
{
    std::shared_ptr<KvmWebSocket> ws;
    {
        std::lock_guard lock(state.control_mutex);
        ws = state.websocket;
        state.websocket.reset();
        state.subprotocol.clear();
        state.send_packet = {};
        state.stop_network = {};
    }

    if (!ws) {
        return;
    }

    beast::error_code error;
    beast::get_lowest_layer(*ws).socket().shutdown(tcp::socket::shutdown_both, error);
    error.clear();
    beast::get_lowest_layer(*ws).socket().close(error);
}

ViewStatusSnapshot status_snapshot(ViewState& state)
{
    ViewStatusSnapshot snapshot;
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

std::uint16_t packet_status_from_bytes(const std::vector<std::uint8_t>& packet)
{
    if (packet.size() < 8) {
        return 0;
    }
    return static_cast<std::uint16_t>(packet[6]) |
           static_cast<std::uint16_t>(packet[7] << 8);
}

std::size_t packet_payload_size_from_bytes(const std::vector<std::uint8_t>& packet)
{
    return packet.size() >= 8 ? packet.size() - 8 : 0;
}

std::string max_session_close_reason(std::uint16_t status)
{
    switch (status) {
    case 0:
        return "maximum KVM sessions reached";
    case 1:
        return "same KVM client/user already connected";
    default:
        return "unknown session close reason";
    }
}

class KvmAsyncSession : public std::enable_shared_from_this<KvmAsyncSession> {
public:
    KvmAsyncSession(
        asio::io_context& io,
        std::shared_ptr<KvmWebSocket> ws,
        KvmViewOptions options,
        KvmConfig config,
        std::string subprotocol,
        ViewState& state)
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
            set_status(self->state_, "stopping");

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
            set_status(state_, "idle timeout");
            std::cerr << "hitsc: kvm view idle timeout after "
                      << options_.idle_timeout_seconds << "s\n";
            close_socket();
            return;
        }

        if (error == websocket::error::closed ||
            error == ssl::error::stream_truncated) {
            set_status(state_, "remote closed");
            std::cerr << "hitsc: kvm websocket closed\n";
            close_socket();
            return;
        }

        set_status(state_, std::string("error: ") + error.message());
        std::cerr << "hitsc: kvm view error: " << error.message()
                  << " [" << error.category().name() << ':' << error.value() << "]\n";
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
                make_validate_packet(config_, options_.login.username),
                false);
            validation_sent_ = true;
            std::cout << "hitsc: sent KVM validation packet\n";
        } else if (packet.type == kCmdKeepAlive) {
            queue_packet_from_strand(kCmdKeepAlive, make_simple_packet(kCmdKeepAlive), false);
        } else if (packet.type == kCmdUsbMouseMode && !packet.payload.empty()) {
            set_mouse_mode(state_, packet.payload[0]);
            if (options_.login.verbose) {
                std::cout << "hitsc: mouse mode=" << static_cast<int>(packet.payload[0]) << '\n';
            }
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
            std::cout << "hitsc: requested full screen\n";
        } else if (packet.type == kCmdMaxSessionClose) {
            const std::string reason = max_session_close_reason(packet.status);
            set_status(state_, "session closed: " + reason);
            std::cerr << "hitsc: kvm session closed: " << reason
                      << " status=" << packet.status << '\n';
            close_socket();
        } else if (packet.type == kCmdVideoPackets) {
            handle_video_packet(packet);
        }

        send_fps_report_if_due();
    }

    void handle_video_packet(const KvmPacket& packet)
    {
        ++video_packets_seen_;
        const std::optional<KvmVideoFrame> frame = video_assembler_.ingest(packet.payload);
        if (!frame) {
            if (options_.login.verbose && video_packets_seen_ <= 20) {
                std::cout << "hitsc: video packet #" << video_packets_seen_
                          << " payload=" << packet.payload.size()
                          << " complete=no\n";
            }
            return;
        }

        const std::uint8_t block_header = kvm_video_first_block_header(frame->compressed);
        if (frame->rc4_enable != 0 || !kvm_video_is_supported_first_block(block_header)) {
            std::cerr << "hitsc: skipped unsupported video frame"
                      << " compression=" << static_cast<int>(frame->compression_mode)
                      << " rc4=" << static_cast<int>(frame->rc4_enable)
                      << " first-block=0x" << std::hex << static_cast<int>(block_header) << std::dec
                      << '\n';
            ++frames_received_since_report_;
            fps_reporting_started_ = true;
            return;
        }

        ++frames_received_since_report_;
        fps_reporting_started_ = true;
        const AspeedDecodeOptions decode_options{
            frame->width,
            frame->height,
            frame->mode420,
            frame->jpeg_table_selector,
            frame->advance_table_selector,
        };
        const int next_frame_number = frames_seen_ + 1;
        if (options_.login.verbose && (next_frame_number <= 20 || next_frame_number % 60 == 0)) {
            std::cout << "hitsc: decoding frame #" << next_frame_number
                      << " packets=" << packets_seen_
                      << " video-packets=" << video_packets_seen_
                      << " size=" << frame->width << "x" << frame->height
                      << " compressed=" << frame->compressed.size()
                      << " compression=" << static_cast<int>(frame->compression_mode)
                      << " mode420=" << static_cast<int>(frame->mode420)
                      << " first-block=0x" << std::hex << static_cast<int>(block_header) << std::dec
                      << '\n';
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
            std::cout << "hitsc: rendered frame #" << frames_seen_
                      << " packets=" << packets_seen_
                      << " video-packets=" << video_packets_seen_
                      << " size=" << frame->width << "x" << frame->height
                      << " compressed=" << frame->compressed.size()
                      << " avg-rgb=" << average_rgb
                      << '\n';
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

            set_status(state_, std::string("error: ") + error.message());
            std::cerr << "hitsc: kvm write error: " << error.message()
                      << " [" << error.category().name() << ':' << error.value() << "]\n";
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
                std::cout << "hitsc: sent stop session packet\n";
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
        set_status(state_, std::string("error: ") + ex.what());
        std::cerr << "hitsc: kvm view error: " << ex.what() << '\n';
        close_socket();
    }

    void close_socket()
    {
        if (closed_) {
            return;
        }

        closed_ = true;
        clear_network_callbacks(state_);
        outgoing_packets_.clear();
        beast::error_code error;
        beast::get_lowest_layer(*ws_).socket().shutdown(tcp::socket::shutdown_both, error);
        error.clear();
        beast::get_lowest_layer(*ws_).socket().close(error);
        if (stopping_) {
            set_status(state_, "stopped");
        }
    }

    asio::strand<asio::io_context::executor_type> strand_;
    std::shared_ptr<KvmWebSocket> ws_;
    KvmViewOptions options_;
    KvmConfig config_;
    std::string subprotocol_;
    ViewState& state_;
    beast::flat_buffer read_buffer_;
    PacketBuffer packet_buffer_;
    AspeedDecoder decoder_;
    KvmVideoAssembler video_assembler_;
    std::vector<std::uint8_t> framebuffer_;
    std::deque<OutgoingPacket> outgoing_packets_;
    int framebuffer_width_ = 0;
    int framebuffer_height_ = 0;
    int packets_seen_ = 0;
    int video_packets_seen_ = 0;
    int frames_seen_ = 0;
    bool validation_sent_ = false;
    bool full_screen_requested_ = false;
    bool fps_reporting_started_ = false;
    bool writing_ = false;
    bool stopping_ = false;
    bool closed_ = false;
    int frames_received_since_report_ = 0;
    int frames_processed_since_report_ = 0;
    std::chrono::steady_clock::time_point last_fps_report_ = std::chrono::steady_clock::now();
};

void network_thread_main(const KvmViewOptions& options, ViewState& state, const std::atomic_bool& stop_requested)
{
    try {
        set_status(state, "logging in");
        MegaRacSession session = login_megarac(options.login);
        std::cout << "hitsc: megarac login succeeded\n";

        set_status(state, "fetching KVM config");
        KvmConfig config = fetch_kvm_config(options.login, session.cookies, session.csrf_token);
        session.cookies.set("__Host-isActiveKVM", "true");
        std::cout << "hitsc: kvm config fetched"
                  << " client_ip=" << config.client_ip
                  << " server_ip=" << config.server_ip
                  << " token=present"
                  << " reconnect=" << (config.reconnect_enabled ? "yes" : "no")
                  << '\n';

        asio::io_context io;
        ssl::context tls_context(ssl::context::tls_client);
        auto ws = std::make_shared<KvmWebSocket>(io, tls_context);
        store_websocket(state, ws);
        tcp::resolver resolver(io);

        configure_tls(tls_context, ws->next_layer(), options.login.base_url.host, options.login.insecure);
        set_server_name_indication(ws->next_layer(), options.login.base_url.host);

        set_status(state, "connecting");
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

            const std::string cookie_header = session.cookies.header();
            if (!cookie_header.empty()) {
                request.set(http::field::cookie, cookie_header);
            }
        }));

        websocket::response_type response;
        ws->handshake(response, host, "/kvm");
        const std::string subprotocol = selected_subprotocol(response);
        set_subprotocol(state, subprotocol);
        set_status(state, "connected");
        std::cout << "hitsc: kvm websocket connected"
                  << " subprotocol=" << subprotocol;
        if (options.idle_timeout_seconds > 0) {
            std::cout << " idle-timeout=" << options.idle_timeout_seconds << "s\n";
        } else {
            std::cout << " idle-timeout=disabled\n";
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
        set_status(state, std::string("error: ") + ex.what());
        std::cerr << "hitsc: kvm view error: " << ex.what() << '\n';
    }

    clear_network_callbacks(state);
    store_websocket(state, {});
}

void throw_sdl_error(std::string_view context)
{
    throw std::runtime_error(std::string(context) + ": " + SDL_GetError());
}

SDL_FRect centered_target_rect(int window_width, int window_height, int frame_width, int frame_height)
{
    const float width_scale = static_cast<float>(window_width) / static_cast<float>(frame_width);
    const float height_scale = static_cast<float>(window_height) / static_cast<float>(frame_height);
    const float scale = std::min(width_scale, height_scale);
    SDL_FRect rect{};
    rect.w = std::floor(static_cast<float>(frame_width) * scale);
    rect.h = std::floor(static_cast<float>(frame_height) * scale);
    rect.x = std::floor((static_cast<float>(window_width) - rect.w) / 2.0f);
    rect.y = std::floor((static_cast<float>(window_height) - rect.h) / 2.0f);
    return rect;
}

SDL_FRect current_target_rect(SDL_Window* window, int frame_width, int frame_height)
{
    int window_width = 0;
    int window_height = 0;
    if (!SDL_GetWindowSizeInPixels(window, &window_width, &window_height)) {
        SDL_GetWindowSize(window, &window_width, &window_height);
    }
    return centered_target_rect(window_width, window_height, frame_width, frame_height);
}

std::optional<RemoteMousePosition> remote_mouse_position(
    float window_x,
    float window_y,
    const SDL_FRect& target,
    int frame_width,
    int frame_height)
{
    if (frame_width <= 0 || frame_height <= 0 || target.w <= 0.0f || target.h <= 0.0f) {
        return std::nullopt;
    }
    if (window_x < target.x || window_y < target.y ||
        window_x > target.x + target.w || window_y > target.y + target.h) {
        return std::nullopt;
    }

    const double normalized_x =
        (static_cast<double>(window_x) - static_cast<double>(target.x)) / static_cast<double>(target.w);
    const double normalized_y =
        (static_cast<double>(window_y) - static_cast<double>(target.y)) / static_cast<double>(target.h);
    return RemoteMousePosition{
        std::clamp(static_cast<int>(std::floor(normalized_x * frame_width + 0.5)), 0, frame_width),
        std::clamp(static_cast<int>(std::floor(normalized_y * frame_height + 0.5)), 0, frame_height),
    };
}

std::uint8_t button_mask_for_sdl_button(std::uint8_t button)
{
    switch (button) {
    case SDL_BUTTON_LEFT:
        return kMouseLeftButton;
    case SDL_BUTTON_RIGHT:
        return kMouseRightButton;
    case SDL_BUTTON_MIDDLE:
        return kMouseMiddleButton;
    default:
        return 0;
    }
}

void send_mouse_report(
    ViewState& state,
    std::uint8_t buttons,
    const RemoteMousePosition& position,
    int frame_width,
    int frame_height,
    int wheel,
    std::optional<RemoteMousePosition>& last_relative_position,
    std::uint32_t& sequence,
    bool verbose)
{
    const int mouse_mode = mouse_mode_snapshot(state);
    std::vector<std::uint8_t> packet;
    if (mouse_mode == kRelativeMouseMode || mouse_mode == kOtherMouseMode) {
        const int dx = last_relative_position ? position.x - last_relative_position->x : 0;
        const int dy = last_relative_position ? position.y - last_relative_position->y : 0;
        packet = make_relative_mouse_packet(buttons, dx, dy, wheel, sequence++);
    } else {
        packet = make_absolute_mouse_packet(buttons, position.x, position.y, frame_width, frame_height, wheel, sequence++);
    }

    last_relative_position = position;
    const bool coalesce = buttons == 0 && wheel == 0;
    const bool accepted = queue_outgoing_packet(state, kCmdSendHidPacket, std::move(packet), coalesce);
    if (verbose && accepted) {
        std::cout << "hitsc: queued mouse"
                  << " mode=" << mouse_mode
                  << " buttons=" << static_cast<int>(buttons)
                  << " x=" << position.x
                  << " y=" << position.y
                  << " wheel=" << wheel
                  << '\n';
    }
}

void stop_network_thread(
    ViewState& state,
    std::atomic_bool& stop_requested,
    std::thread& network_thread,
    const std::atomic_bool& network_done)
{
    stop_requested.store(true);
    cancel_network(state);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    while (!network_done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!network_thread.joinable()) {
        return;
    }
    if (network_done.load()) {
        network_thread.join();
    } else {
        network_thread.detach();
    }
}

} // namespace

void run_kvm_view(const KvmViewOptions& options)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw_sdl_error("SDL_Init");
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;

    auto stop_requested = std::make_shared<std::atomic_bool>(false);
    auto network_done = std::make_shared<std::atomic_bool>(false);
    auto state = std::make_shared<ViewState>();
    std::thread network_thread;

    try {
        window = SDL_CreateWindow("hitsc", 1024, 768, SDL_WINDOW_RESIZABLE);
        if (window == nullptr) {
            throw_sdl_error("SDL_CreateWindow");
        }

        renderer = SDL_CreateRenderer(window, nullptr);
        if (renderer == nullptr) {
            throw_sdl_error("SDL_CreateRenderer");
        }

        KvmViewOptions network_options = options;
        network_thread = std::thread([network_options, state, stop_requested, network_done] {
            network_thread_main(network_options, *state, *stop_requested);
            network_done->store(true);
        });

        bool running = true;
        std::uint64_t last_sequence = 0;
        std::uint64_t last_status_tick = 0;
        int presented_frames = 0;
        int texture_width = 0;
        int texture_height = 0;
        std::uint8_t mouse_buttons = 0;
        std::uint32_t mouse_sequence = 0;
        std::uint64_t last_mouse_motion_ticks = 0;
        std::optional<RemoteMousePosition> last_relative_mouse_position;

        while (running) {
            if (network_done->load()) {
                running = false;
                continue;
            }

            SDL_Event event{};
            std::optional<RemoteMousePosition> pending_mouse_motion;
            std::uint64_t pending_mouse_motion_ticks = 0;
            int events_processed = 0;
            while (events_processed < kMaxSdlEventsPerFrame && SDL_PollEvent(&event)) {
                ++events_processed;
                if (event.type == SDL_EVENT_QUIT ||
                    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    running = false;
                } else if (event.type == SDL_EVENT_KEY_DOWN) {
                    if (options.login.verbose) {
                        std::cout << "hitsc: key down scancode=" << event.key.scancode
                                  << " key=" << event.key.key << '\n';
                    }
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                           event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    if (texture_width > 0 && texture_height > 0) {
                        const std::uint8_t mask = button_mask_for_sdl_button(event.button.button);
                        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                            mouse_buttons |= mask;
                        } else {
                            mouse_buttons &= static_cast<std::uint8_t>(~mask);
                        }
                        if (mouse_buttons != 0) {
                            SDL_CaptureMouse(true);
                        } else {
                            SDL_CaptureMouse(false);
                        }

                        const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                        const std::optional<RemoteMousePosition> position = remote_mouse_position(
                            event.button.x,
                            event.button.y,
                            target,
                            texture_width,
                            texture_height);
                        if (position) {
                            send_mouse_report(
                                *state,
                                mouse_buttons,
                                *position,
                                texture_width,
                                texture_height,
                                0,
                                last_relative_mouse_position,
                                mouse_sequence,
                                options.login.verbose);
                        }
                    }
                } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    if (texture_width > 0 && texture_height > 0) {
                        const std::uint64_t ticks = SDL_GetTicks();
                        if (mouse_buttons == 0 &&
                            ticks - last_mouse_motion_ticks < kMouseMotionIntervalMilliseconds) {
                            continue;
                        }

                        const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                        const std::optional<RemoteMousePosition> position = remote_mouse_position(
                            event.motion.x,
                            event.motion.y,
                            target,
                            texture_width,
                            texture_height);
                        if (position) {
                            pending_mouse_motion = *position;
                            pending_mouse_motion_ticks = ticks;
                        }
                    }
                } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                    if (texture_width > 0 && texture_height > 0) {
                        const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                        const std::optional<RemoteMousePosition> position = remote_mouse_position(
                            event.wheel.mouse_x,
                            event.wheel.mouse_y,
                            target,
                            texture_width,
                            texture_height);
                        if (position) {
                            const int wheel = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
                                ? static_cast<int>(-event.wheel.y)
                                : static_cast<int>(event.wheel.y);
                            send_mouse_report(
                                *state,
                                mouse_buttons,
                                *position,
                                texture_width,
                                texture_height,
                                wheel,
                                last_relative_mouse_position,
                                mouse_sequence,
                                options.login.verbose);
                        }
                    }
                }
            }

            if (pending_mouse_motion) {
                send_mouse_report(
                    *state,
                    mouse_buttons,
                    *pending_mouse_motion,
                    texture_width,
                    texture_height,
                    0,
                    last_relative_mouse_position,
                    mouse_sequence,
                    options.login.verbose);
                last_mouse_motion_ticks = pending_mouse_motion_ticks;
            }

            const std::optional<SharedFrame> frame = take_latest_frame(*state, last_sequence);
            if (frame) {
                last_sequence = frame->sequence;
                if (texture == nullptr || texture_width != frame->width || texture_height != frame->height) {
                    if (texture != nullptr) {
                        SDL_DestroyTexture(texture);
                    }
                    texture = SDL_CreateTexture(
                        renderer,
                        SDL_PIXELFORMAT_RGBA32,
                        SDL_TEXTUREACCESS_STREAMING,
                        frame->width,
                        frame->height);
                    if (texture == nullptr) {
                        throw_sdl_error("SDL_CreateTexture");
                    }
                    texture_width = frame->width;
                    texture_height = frame->height;
                    SDL_SetWindowTitle(
                        window,
                        ("hitsc - " + options.login.base_url.host + " - "
                         + std::to_string(frame->width) + "x" + std::to_string(frame->height))
                            .c_str());
                }

                if (!SDL_UpdateTexture(texture, nullptr, frame->rgba.data(), frame->width * 4)) {
                    throw_sdl_error("SDL_UpdateTexture");
                }

                ++presented_frames;
                if (options.login.verbose && (presented_frames <= 20 || presented_frames % 60 == 0)) {
                    std::cout << "hitsc: presented frame #" << presented_frames
                              << " sequence=" << frame->sequence
                              << " avg-rgb=" << sampled_average_rgb(frame->rgba)
                              << '\n';
                }
            }

            SDL_SetRenderDrawColor(renderer, 12, 14, 18, 255);
            SDL_RenderClear(renderer);
            if (texture != nullptr) {
                const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                SDL_RenderTexture(renderer, texture, nullptr, &target);
            }
            SDL_RenderPresent(renderer);

            const std::uint64_t ticks = SDL_GetTicks();
            if (texture == nullptr && ticks - last_status_tick > 1000) {
                last_status_tick = ticks;
                const ViewStatusSnapshot snapshot = status_snapshot(*state);
                SDL_SetWindowTitle(window, ("hitsc - " + snapshot.status).c_str());
            }
            SDL_Delay(16);
        }
    } catch (...) {
        stop_network_thread(*state, *stop_requested, network_thread, *network_done);
        if (texture != nullptr) {
            SDL_DestroyTexture(texture);
        }
        if (renderer != nullptr) {
            SDL_DestroyRenderer(renderer);
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
        throw;
    }

    stop_network_thread(*state, *stop_requested, network_thread, *network_done);

    if (texture != nullptr) {
        SDL_DestroyTexture(texture);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

} // namespace hitsc
