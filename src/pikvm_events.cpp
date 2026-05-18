#include "pikvm_events.hpp"

#include "app_info.hpp"
#include "log.hpp"
#include "pikvm_session.hpp"
#include "text.hpp"
#include "tls.hpp"
#include "url.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json.hpp>
#include <boost/version.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
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
using PikvmWebSocket = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

namespace {

std::string buffer_text(const beast::flat_buffer& buffer)
{
    std::string text(boost::asio::buffer_size(buffer.data()), '\0');
    boost::asio::buffer_copy(boost::asio::buffer(text), buffer.data());
    return text;
}

std::vector<std::uint8_t> buffer_bytes(const beast::flat_buffer& buffer)
{
    std::vector<std::uint8_t> bytes(boost::asio::buffer_size(buffer.data()));
    boost::asio::buffer_copy(boost::asio::buffer(bytes), buffer.data());
    return bytes;
}

std::string printable_preview(std::string_view value, std::size_t limit = 200)
{
    std::string preview;
    const std::size_t clipped = std::min(value.size(), limit);
    for (std::size_t i = 0; i < clipped; ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (ch == '\n') {
            preview += "\\n";
        } else if (ch == '\r') {
            preview += "\\r";
        } else if (ch == '\t') {
            preview += "\\t";
        } else if (ch == '"') {
            preview += "\\\"";
        } else if (ch == '\\') {
            preview += "\\\\";
        } else if (std::isprint(ch)) {
            preview.push_back(static_cast<char>(ch));
        } else {
            preview.push_back('.');
        }
    }
    if (value.size() > clipped) {
        preview += "...";
    }
    return preview;
}

std::string hex_preview(const std::vector<std::uint8_t>& bytes, std::size_t limit = 24)
{
    std::ostringstream output;
    const std::size_t clipped = std::min(bytes.size(), limit);
    for (std::size_t i = 0; i < clipped; ++i) {
        if (i != 0) {
            output << ' ';
        }
        output << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(bytes[i])
               << std::dec << std::setfill(' ');
    }
    if (bytes.size() > clipped) {
        output << " ...";
    }
    return output.str();
}

std::string json_event_type(std::string_view text)
{
    boost::system::error_code error;
    json::value value = json::parse(text, error);
    if (error) {
        return {};
    }

    const json::object* object = value.if_object();
    if (object == nullptr) {
        return {};
    }

    const json::value* event_type = object->if_contains("event_type");
    if (event_type == nullptr) {
        return {};
    }

    const json::string* event_type_string = event_type->if_string();
    if (event_type_string == nullptr) {
        return {};
    }

    return std::string(*event_type_string);
}

void print_text_event_summary(const std::string& text, std::size_t bytes_transferred)
{
    const std::string event_type = json_event_type(text);
    std::cout << "PiKVM websocket event";
    if (!event_type.empty()) {
        std::cout << " event_type=" << event_type;
    } else {
        std::cout << " event_type=<unparsed>";
    }
    std::cout << " bytes=" << bytes_transferred
              << " preview=\"" << printable_preview(text) << "\"\n";
}

void print_binary_event_summary(const beast::flat_buffer& buffer, std::size_t bytes_transferred)
{
    const std::vector<std::uint8_t> bytes = buffer_bytes(buffer);
    std::cout << "PiKVM websocket event type=binary"
              << " bytes=" << bytes_transferred;
    if (!bytes.empty()) {
        std::cout << " first-bytes=" << hex_preview(bytes);
    }
    std::cout << '\n';
}

} // namespace

void run_pikvm_events_probe(const PikvmProbeOptions& options)
{
    log_info() << "pikvm login starting";
    PikvmSession session = login_pikvm(options.login);
    PikvmLogoutGuard logout_guard(options.login);
    logout_guard.arm(session);
    log_info() << "pikvm login succeeded";
    if (options.login.verbose) {
        log_info() << "cookies stored: " << session.cookies.size();
    }

    asio::io_context io;
    ssl::context tls_context(ssl::context::tls_client);
    PikvmWebSocket ws(io, tls_context);
    tcp::resolver resolver(io);

    configure_tls(tls_context, ws.next_layer(), options.login.base_url.host, options.login.insecure);
    set_server_name_indication(ws.next_layer(), options.login.base_url.host);

    const auto endpoints = resolver.resolve(options.login.base_url.host, options.login.base_url.port);
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(ws).connect(endpoints);
    beast::get_lowest_layer(ws).socket().set_option(tcp::no_delay(true));
    ws.next_layer().handshake(ssl::stream_base::client);
    beast::get_lowest_layer(ws).expires_never();

    websocket::stream_base::timeout timeout;
    timeout.handshake_timeout = std::chrono::seconds(30);
    if (options.idle_timeout_seconds > 0) {
        timeout.idle_timeout = std::chrono::seconds(options.idle_timeout_seconds);
        timeout.keep_alive_pings = true;
    } else {
        timeout.idle_timeout = websocket::stream_base::none();
        timeout.keep_alive_pings = false;
    }
    ws.set_option(timeout);

    const std::string host = make_host_header(options.login.base_url);
    ws.set_option(websocket::stream_base::decorator([&](websocket::request_type& request) {
        request.set(http::field::user_agent, std::string(kName) + "/" + std::string(BOOST_LIB_VERSION));
        request.set(http::field::origin, make_origin(options.login.base_url));

        const std::string cookie_header = session.cookies.header();
        if (!cookie_header.empty()) {
            request.set(http::field::cookie, cookie_header);
        }
    }));

    const std::string path = "/api/ws?stream=0";
    websocket::response_type response;
    ws.handshake(response, host, path);
    {
        LogLine line = log_info();
        line << "pikvm websocket connected"
             << " path=" << path;
        if (options.idle_timeout_seconds > 0) {
            line << " idle-timeout=" << options.idle_timeout_seconds << "s";
        } else {
            line << " idle-timeout=disabled";
        }
    }

    beast::flat_buffer buffer;
    const std::size_t bytes_transferred = ws.read(buffer);
    if (ws.got_text()) {
        print_text_event_summary(buffer_text(buffer), bytes_transferred);
    } else {
        print_binary_event_summary(buffer, bytes_transferred);
    }

    beast::error_code close_error;
    ws.close(websocket::close_code::normal, close_error);
    if (close_error && options.login.verbose) {
        log_warning() << "pikvm websocket close warning: " << close_error.message();
    }
}

} // namespace hitsc
