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

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
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

constexpr std::uint16_t kCmdConnectionAllowed = 23;
constexpr std::uint16_t kCmdVideoPackets = 25;
constexpr std::uint16_t kCmdActiveClients = 39;
constexpr std::uint16_t kCmdKeepAlive = 57;
constexpr std::uint16_t kCmdValidateVideoSession = 18;
constexpr std::uint16_t kCmdValidatedVideoSession = 19;
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

struct ViewState {
    std::mutex mutex;
    SharedFrame frame;
    std::string status = "starting";
    std::shared_ptr<KvmWebSocket> websocket;
    bool has_frame = false;
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

void write_packet(KvmWebSocket& ws, const std::vector<std::uint8_t>& packet, std::string_view subprotocol)
{
    if (is_binary_mode(subprotocol)) {
        ws.binary(true);
        ws.write(asio::buffer(packet));
        return;
    }

    ws.text(true);
    const std::string encoded = base64_encode(packet);
    ws.write(asio::buffer(encoded));
}

std::vector<std::uint8_t> read_message_bytes(
    KvmWebSocket& ws,
    beast::flat_buffer& buffer,
    std::string_view subprotocol)
{
    buffer.clear();
    ws.read(buffer);

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
    std::lock_guard lock(state.mutex);
    state.status = std::move(status);
}

void publish_frame(ViewState& state, int width, int height, std::vector<std::uint8_t> rgba)
{
    std::lock_guard lock(state.mutex);
    state.frame.width = width;
    state.frame.height = height;
    state.frame.rgba = std::move(rgba);
    ++state.frame.sequence;
    state.has_frame = true;
}

std::optional<SharedFrame> take_latest_frame(ViewState& state, std::uint64_t last_sequence)
{
    std::lock_guard lock(state.mutex);
    if (!state.has_frame || state.frame.sequence == last_sequence) {
        return std::nullopt;
    }
    return state.frame;
}

std::string packet_name(std::uint16_t type)
{
    switch (type) {
    case 1:
        return "CMD_SEND_HID_PACKET";
    case 4:
        return "CMD_PAUSE_REDIRECTION";
    case kCmdResumeRedirection:
        return "CMD_RESUME_REDIRECTION";
    case 8:
        return "CMD_STOP_SESSION_IMMEDIATE";
    case kCmdUsbMouseMode:
        return "CMD_USB_MOUSE_MODE";
    case kCmdGetFullScreen:
        return "CMD_GET_FULL_SCREEN";
    case kCmdValidateVideoSession:
        return "CMD_VALIDATE_VIDEO_SESSION";
    case kCmdValidatedVideoSession:
        return "CMD_VALIDATED_VIDEO_SESSION";
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
    std::lock_guard lock(state.mutex);
    state.websocket = std::move(ws);
}

void cancel_network(ViewState& state)
{
    std::shared_ptr<KvmWebSocket> ws;
    {
        std::lock_guard lock(state.mutex);
        ws = state.websocket;
    }

    if (!ws) {
        return;
    }

    beast::error_code error;
    beast::get_lowest_layer(*ws).socket().cancel(error);
    error.clear();
    beast::get_lowest_layer(*ws).socket().close(error);
}

ViewStatusSnapshot status_snapshot(ViewState& state)
{
    std::lock_guard lock(state.mutex);
    return ViewStatusSnapshot{state.status, state.has_frame};
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

        websocket::stream_base::timeout timeout;
        timeout.handshake_timeout = std::chrono::seconds(30);
        timeout.idle_timeout = std::chrono::seconds(options.idle_timeout_seconds);
        timeout.keep_alive_pings = true;
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
        set_status(state, "connected");
        std::cout << "hitsc: kvm websocket connected"
                  << " subprotocol=" << subprotocol
                  << " idle-timeout=" << options.idle_timeout_seconds << "s\n";

        PacketBuffer packet_buffer;
        beast::flat_buffer read_buffer;
        AspeedDecoder decoder;
        KvmVideoAssembler video_assembler;
        std::vector<std::uint8_t> framebuffer;
        int framebuffer_width = 0;
        int framebuffer_height = 0;
        int packets_seen = 0;
        int video_packets_seen = 0;
        int frames_seen = 0;
        bool validation_sent = false;
        bool full_screen_requested = false;
        bool fps_reporting_started = false;
        int frames_received_since_report = 0;
        int frames_processed_since_report = 0;
        int fps_reports_sent = 0;
        auto last_fps_report = std::chrono::steady_clock::now();

        while (!stop_requested.load()) {
            std::vector<std::uint8_t> bytes;
            try {
                bytes = read_message_bytes(*ws, read_buffer, subprotocol);
            } catch (const beast::system_error& error) {
                if (stop_requested.load()) {
                    break;
                }
                if (error.code() == beast::error::timeout) {
                    set_status(state, "idle timeout");
                    std::cerr << "hitsc: kvm view idle timeout after "
                              << options.idle_timeout_seconds << "s\n";
                    break;
                }
                if (error.code() == websocket::error::closed ||
                    error.code() == ssl::error::stream_truncated ||
                    error.code() == boost::asio::error::operation_aborted ||
                    error.code() == boost::asio::error::bad_descriptor) {
                    set_status(state, "remote closed");
                    std::cerr << "hitsc: kvm websocket closed\n";
                    break;
                }
                throw;
            }

            packet_buffer.append(bytes);
            while (std::optional<KvmPacket> packet = packet_buffer.next()) {
                ++packets_seen;
                if (options.login.verbose) {
                    log_packet(packets_seen, *packet);
                }

                if (packet->type == kCmdConnectionAllowed && !validation_sent) {
                    const std::vector<std::uint8_t> validate = make_validate_packet(config, options.login.username);
                    write_packet(*ws, validate, subprotocol);
                    validation_sent = true;
                    std::cout << "hitsc: sent KVM validation packet\n";
                } else if (packet->type == kCmdKeepAlive) {
                    write_packet(*ws, make_simple_packet(kCmdKeepAlive), subprotocol);
                } else if (packet->type == kCmdMediaLicenseStatus) {
                    const std::vector<std::uint8_t> display_lock =
                        make_payload_packet(kCmdDisplayLockSet, 0, std::vector<std::uint8_t>{2});
                    write_packet(*ws, display_lock, subprotocol);
                    log_sent_packet(kCmdDisplayLockSet, 0, 1, options.login.verbose);

                    const std::vector<std::uint8_t> user_macro = make_simple_packet(kCmdGetUserMacro);
                    write_packet(*ws, user_macro, subprotocol);
                    log_sent_packet(kCmdGetUserMacro, 0, 0, options.login.verbose);

                    if (!config.session.empty()) {
                        const std::vector<std::uint8_t> web_token = make_web_token_packet(config.session);
                        write_packet(*ws, web_token, subprotocol);
                        log_sent_packet(kCmdGetWebToken, 0, kWebTokenPayloadLength, options.login.verbose);
                    }
                } else if (packet->type == kCmdActiveClients && !full_screen_requested) {
                    write_packet(*ws, make_simple_packet(kCmdGetFullScreen, 1), subprotocol);
                    full_screen_requested = true;
                    std::cout << "hitsc: requested full screen\n";
                } else if (packet->type == kCmdVideoPackets) {
                    ++video_packets_seen;
                    const std::optional<KvmVideoFrame> frame = video_assembler.ingest(packet->payload);
                    if (!frame) {
                        if (video_packets_seen <= 20) {
                            std::cout << "hitsc: video packet #" << video_packets_seen
                                      << " payload=" << packet->payload.size()
                                      << " complete=no\n";
                        }
                        continue;
                    }

                    const std::uint8_t block_header = kvm_video_first_block_header(frame->compressed);
                    if (frame->rc4_enable != 0 || !kvm_video_is_supported_first_block(block_header)) {
                        std::cerr << "hitsc: skipped unsupported video frame"
                                  << " compression=" << static_cast<int>(frame->compression_mode)
                                  << " rc4=" << static_cast<int>(frame->rc4_enable)
                                  << " first-block=0x" << std::hex << static_cast<int>(block_header) << std::dec
                                  << '\n';
                        ++frames_received_since_report;
                        fps_reporting_started = true;
                        continue;
                    }

                    ++frames_received_since_report;
                    fps_reporting_started = true;
                    const AspeedDecodeOptions decode_options{
                        frame->width,
                        frame->height,
                        frame->mode420,
                        frame->jpeg_table_selector,
                        frame->advance_table_selector,
                    };
                    const int next_frame_number = frames_seen + 1;
                    if (next_frame_number <= 20 || next_frame_number % 60 == 0) {
                        std::cout << "hitsc: decoding frame #" << next_frame_number
                                  << " packets=" << packets_seen
                                  << " video-packets=" << video_packets_seen
                                  << " size=" << frame->width << "x" << frame->height
                                  << " compressed=" << frame->compressed.size()
                                  << " compression=" << static_cast<int>(frame->compression_mode)
                                  << " mode420=" << static_cast<int>(frame->mode420)
                                  << " first-block=0x" << std::hex << static_cast<int>(block_header) << std::dec
                                  << '\n';
                    }
                    const bool can_use_previous =
                        framebuffer_width == frame->width
                        && framebuffer_height == frame->height
                        && framebuffer.size()
                            == static_cast<std::size_t>(frame->width) * static_cast<std::size_t>(frame->height) * 4;

                    std::vector<std::uint8_t> rgba =
                        decoder.decode_rgba(decode_options, frame->compressed, can_use_previous ? &framebuffer : nullptr);
                    const int average_rgb = sampled_average_rgb(rgba);
                    framebuffer = rgba;
                    framebuffer_width = frame->width;
                    framebuffer_height = frame->height;
                    publish_frame(state, frame->width, frame->height, std::move(rgba));
                    ++frames_processed_since_report;

                    ++frames_seen;
                    if (frames_seen <= 20 || frames_seen % 60 == 0) {
                        std::cout << "hitsc: rendered frame #" << frames_seen
                                  << " packets=" << packets_seen
                                  << " video-packets=" << video_packets_seen
                                  << " size=" << frame->width << "x" << frame->height
                                  << " compressed=" << frame->compressed.size()
                                  << " avg-rgb=" << average_rgb
                                  << '\n';
                    }
                }

                const auto now = std::chrono::steady_clock::now();
                if (fps_reporting_started && now - last_fps_report >= std::chrono::milliseconds(100)) {
                    const int diff = std::abs(frames_received_since_report - frames_processed_since_report);
                    write_packet(*ws, make_simple_packet(kCmdFpsDiff, static_cast<std::uint16_t>(diff)), subprotocol);
                    ++fps_reports_sent;
                    if (options.login.verbose && (fps_reports_sent <= 5 || fps_reports_sent % 50 == 0)) {
                        log_sent_packet(kCmdFpsDiff, static_cast<std::uint16_t>(diff), 0, true);
                    }
                    frames_received_since_report = 0;
                    frames_processed_since_report = 0;
                    last_fps_report = now;
                }
            }
        }

        if (!stop_requested.load() && ws->is_open()) {
            beast::error_code close_error;
            ws->close(websocket::close_code::normal, close_error);
        }
        set_status(state, "stopped");
    } catch (const std::exception& ex) {
        set_status(state, std::string("error: ") + ex.what());
        std::cerr << "hitsc: kvm view error: " << ex.what() << '\n';
    }

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
        network_thread = std::thread([network_options, state, stop_requested] {
            network_thread_main(network_options, *state, *stop_requested);
        });

        bool running = true;
        std::uint64_t last_sequence = 0;
        std::uint64_t last_status_tick = 0;
        int presented_frames = 0;
        int texture_width = 0;
        int texture_height = 0;

        while (running) {
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT ||
                    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    running = false;
                } else if (event.type == SDL_EVENT_KEY_DOWN) {
                    std::cout << "hitsc: key down scancode=" << event.key.scancode
                              << " key=" << event.key.key << '\n';
                    if (event.key.key == SDLK_ESCAPE) {
                        running = false;
                    }
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                           event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    std::cout << "hitsc: mouse button "
                              << (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "down" : "up")
                              << " button=" << static_cast<int>(event.button.button)
                              << " x=" << event.button.x
                              << " y=" << event.button.y << '\n';
                }
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
                if (presented_frames <= 20 || presented_frames % 60 == 0) {
                    std::cout << "hitsc: presented frame #" << presented_frames
                              << " sequence=" << frame->sequence
                              << " avg-rgb=" << sampled_average_rgb(frame->rgba)
                              << '\n';
                }
            }

            SDL_SetRenderDrawColor(renderer, 12, 14, 18, 255);
            SDL_RenderClear(renderer);
            if (texture != nullptr) {
                int window_width = 0;
                int window_height = 0;
                if (!SDL_GetWindowSizeInPixels(window, &window_width, &window_height)) {
                    SDL_GetWindowSize(window, &window_width, &window_height);
                }
                const SDL_FRect target =
                    centered_target_rect(window_width, window_height, texture_width, texture_height);
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
        stop_requested->store(true);
        cancel_network(*state);
        if (network_thread.joinable()) {
            network_thread.detach();
        }
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

    stop_requested->store(true);
    cancel_network(*state);
    if (network_thread.joinable()) {
        network_thread.detach();
    }

    if (texture != nullptr) {
        SDL_DestroyTexture(texture);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

} // namespace hitsc
