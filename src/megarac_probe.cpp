#include "megarac_probe.hpp"

#include "app_info.hpp"
#include "http_client.hpp"
#include "megarac_capture.hpp"
#include "megarac_session.hpp"
#include "text.hpp"
#include "tls.hpp"
#include "url.hpp"

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
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hitsc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace json = boost::json;
namespace ssl = asio::ssl;
namespace websocket = boost::beast::websocket;
using tcp = asio::ip::tcp;

namespace {

constexpr std::uint16_t kCmdConnectionAllowed = 23;
constexpr std::uint16_t kCmdVideoPackets = 25;
constexpr std::uint16_t kCmdActiveClients = 39;
constexpr std::uint16_t kCmdKeepAlive = 57;
constexpr std::uint16_t kCmdValidateVideoSession = 18;
constexpr std::uint16_t kCmdValidatedVideoSession = 19;
constexpr std::uint16_t kCmdGetFullScreen = 11;
constexpr std::uint16_t kCmdResumeRedirection = 6;

constexpr std::size_t kSsiHashSize = 129;
constexpr std::size_t kClientUsernameLength = 129;
constexpr std::size_t kClientOwnIpLength = 65;
constexpr std::size_t kClientOwnMacLength = 49;
constexpr std::size_t kVideoPacketSize = 373;

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
    case 9:
        return "CMD_PAINT_BLANK_SCREEN";
    case kCmdGetFullScreen:
        return "CMD_GET_FULL_SCREEN";
    case kCmdValidateVideoSession:
        return "CMD_VALIDATE_VIDEO_SESSION";
    case kCmdValidatedVideoSession:
        return "CMD_VALIDATED_VIDEO_SESSION";
    case 22:
        return "CMD_MAX_SESSION_CLOSE";
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
    case 53:
        return "CMD_MEDIA_LICENSE_STATUS";
    case kCmdKeepAlive:
        return "CMD_KEEP_ALIVE_PKT";
    case 58:
        return "CMD_CONNECTION_COMPLETE_PKT";
    case 59:
        return "CMD_CONNECTION_FAILED";
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

        const std::string feature = json_string_field(*object, "feature");
        if (feature == "KVM_SESSION_RECONNECT") {
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

std::vector<std::uint8_t> make_simple_packet(
    std::uint16_t type,
    std::uint16_t status = 0)
{
    return make_header(type, 0, status);
}

std::vector<std::uint8_t> make_validate_packet(
    const KvmConfig& config,
    std::string_view username)
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

void write_packet(
    websocket::stream<beast::ssl_stream<beast::tcp_stream>>& ws,
    const std::vector<std::uint8_t>& packet,
    std::string_view subprotocol)
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
    websocket::stream<beast::ssl_stream<beast::tcp_stream>>& ws,
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

std::string selected_subprotocol(const websocket::response_type& response)
{
    const auto field = response.find(http::field::sec_websocket_protocol);
    if (field == response.end()) {
        return "base64";
    }

    std::string protocol = trim_copy(field->value());
    return protocol.empty() ? "base64" : protocol;
}

std::string make_capture_metadata(
    const LoginOptions& options,
    const KvmConfig& config,
    std::string_view subprotocol)
{
    json::object metadata;
    metadata["format"] = "hitsc-kvm-capture";
    metadata["format_version"] = 1;
    metadata["host"] = options.base_url.host;
    metadata["port"] = options.base_url.port;
    metadata["client_ip"] = config.client_ip;
    metadata["server_ip"] = config.server_ip;
    metadata["subprotocol"] = subprotocol;
    metadata["reconnect_enabled"] = config.reconnect_enabled;
    metadata["token_present"] = !config.token.empty();
    metadata["session_present"] = !config.session.empty();
    metadata["username_present"] = !options.username.empty();
    return json::serialize(metadata);
}

} // namespace

void run_megarac_probe(const MegaracProbeOptions& options)
{
    MegaRacSession session = login_megarac(options.login);
    MegaRacLogoutGuard logout_guard(options.login);
    logout_guard.arm(session);
    std::cout << "hitsc: megarac login succeeded\n";

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
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(io, tls_context);
    tcp::resolver resolver(io);

    configure_tls(tls_context, ws.next_layer(), options.login.base_url.host, options.login.insecure);
    set_server_name_indication(ws.next_layer(), options.login.base_url.host);

    const auto endpoints = resolver.resolve(options.login.base_url.host, options.login.base_url.port);
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(ws).connect(endpoints);
    ws.next_layer().handshake(ssl::stream_base::client);
    beast::get_lowest_layer(ws).expires_never();

    websocket::stream_base::timeout timeout;
    timeout.handshake_timeout = std::chrono::seconds(30);
    timeout.idle_timeout = std::chrono::seconds(options.idle_timeout_seconds);
    timeout.keep_alive_pings = true;
    ws.set_option(timeout);

    const std::string host = make_host_header(options.login.base_url);
    ws.set_option(websocket::stream_base::decorator([&](websocket::request_type& request) {
        request.set(http::field::user_agent, std::string(kName) + "/" + std::string(BOOST_LIB_VERSION));
        request.set(http::field::origin, make_origin(options.login.base_url));
        request.set(http::field::sec_websocket_protocol, "binary, base64");

        const std::string cookie_header = session.cookies.header();
        if (!cookie_header.empty()) {
            request.set(http::field::cookie, cookie_header);
        }
    }));

    websocket::response_type response;
    ws.handshake(response, host, "/kvm");
    const std::string subprotocol = selected_subprotocol(response);
    std::cout << "hitsc: kvm websocket connected"
              << " subprotocol=" << subprotocol
              << " idle-timeout=" << options.idle_timeout_seconds << "s\n";

    std::unique_ptr<MegaracCaptureWriter> capture;
    if (!options.capture_path.empty()) {
        capture = std::make_unique<MegaracCaptureWriter>(options.capture_path);
        capture->write_metadata(make_capture_metadata(options.login, config, subprotocol));
        std::cout << "hitsc: writing KVM capture to " << options.capture_path << '\n';
    }

    PacketBuffer packet_buffer;
    beast::flat_buffer read_buffer;
    int packets_seen = 0;
    bool validation_sent = false;
    bool full_screen_requested = false;

    while (options.packet_limit <= 0 || packets_seen < options.packet_limit) {
        std::vector<std::uint8_t> bytes;
        try {
            bytes = read_message_bytes(ws, read_buffer, subprotocol);
        } catch (const beast::system_error& error) {
            if (error.code() == beast::error::timeout) {
                std::cout << "hitsc: kvm probe idle timeout after "
                          << options.idle_timeout_seconds << "s"
                          << " packets=" << packets_seen << '\n';
                if (capture) {
                    capture->write_note("idle timeout");
                }
                break;
            }
            if (error.code() == websocket::error::closed ||
                error.code() == ssl::error::stream_truncated) {
                std::cout << "hitsc: kvm websocket closed by remote"
                          << " packets=" << packets_seen << '\n';
                if (capture) {
                    capture->write_note("remote closed websocket");
                }
                break;
            }
            throw;
        }

        packet_buffer.append(bytes);

        while (std::optional<KvmPacket> packet = packet_buffer.next()) {
            ++packets_seen;
            if (capture) {
                capture->write_incoming_packet(packet->type, packet->status, packet->payload);
            }
            log_packet(packets_seen, *packet);

            if (packet->type == kCmdConnectionAllowed && !validation_sent) {
                const std::vector<std::uint8_t> validate = make_validate_packet(config, options.login.username);
                write_packet(ws, validate, subprotocol);
                if (capture) {
                    capture->write_outgoing_message(validate);
                }
                validation_sent = true;
                std::cout << "hitsc: sent KVM validation packet\n";
            } else if (packet->type == kCmdKeepAlive) {
                const std::vector<std::uint8_t> keepalive = make_simple_packet(kCmdKeepAlive);
                write_packet(ws, keepalive, subprotocol);
                if (capture) {
                    capture->write_outgoing_message(keepalive);
                }
                std::cout << "hitsc: replied to KVM keepalive\n";
            } else if ((packet->type == kCmdValidatedVideoSession || packet->type == kCmdActiveClients) &&
                       !full_screen_requested) {
                const std::vector<std::uint8_t> full_screen = make_simple_packet(kCmdGetFullScreen, 1);
                write_packet(ws, full_screen, subprotocol);
                if (capture) {
                    capture->write_outgoing_message(full_screen);
                }
                full_screen_requested = true;
                std::cout << "hitsc: requested full screen\n";
            }

            if (options.packet_limit > 0 && packets_seen >= options.packet_limit) {
                break;
            }
        }
    }

    if (ws.is_open()) {
        beast::error_code close_error;
        ws.close(websocket::close_code::normal, close_error);
        if (close_error == beast::error::timeout) {
            close_error = {};
        }
        if (close_error) {
            std::cerr << "hitsc: kvm websocket close warning: " << close_error.message() << '\n';
        }
    }
}

} // namespace hitsc
