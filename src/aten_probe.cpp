#include "aten_probe.hpp"

#include "app_info.hpp"
#include "aten_session.hpp"
#include "tls.hpp"
#include "url.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/version.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace hitsc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = asio::ssl;
namespace websocket = boost::beast::websocket;
using tcp = asio::ip::tcp;

namespace {

std::string normalized_websocket_path(std::string path)
{
    if (path.empty()) {
        return "/";
    }
    if (path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    return path;
}

std::vector<std::uint8_t> buffer_bytes(const beast::flat_buffer& buffer)
{
    std::vector<std::uint8_t> bytes(boost::asio::buffer_size(buffer.data()));
    boost::asio::buffer_copy(boost::asio::buffer(bytes), buffer.data());
    return bytes;
}

bool is_printable_ascii(std::uint8_t byte)
{
    return byte == '\r' || byte == '\n' || byte == '\t' ||
           (byte >= 0x20 && byte <= 0x7e);
}

std::string escaped_ascii(const std::vector<std::uint8_t>& bytes)
{
    std::string text;
    for (const std::uint8_t byte : bytes) {
        if (byte == '\r') {
            text += "\\r";
        } else if (byte == '\n') {
            text += "\\n";
        } else if (byte == '\t') {
            text += "\\t";
        } else if (is_printable_ascii(byte)) {
            text.push_back(static_cast<char>(byte));
        } else {
            return {};
        }
    }
    return text;
}

void print_hex_preview(const std::vector<std::uint8_t>& bytes)
{
    const std::size_t preview = std::min<std::size_t>(bytes.size(), 64);
    std::cout << "hitsc: aten websocket first-bytes=";
    for (std::size_t i = 0; i < preview; ++i) {
        if (i != 0) {
            std::cout << ' ';
        }
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(bytes[i])
                  << std::dec << std::setfill(' ');
    }
    if (bytes.size() > preview) {
        std::cout << " ...";
    }
    std::cout << '\n';
}

} // namespace

void run_aten_probe(const AtenProbeOptions& options)
{
    AtenSession session = login_aten(options.login);
    AtenLogoutGuard logout_guard(options.login);
    logout_guard.arm(session);
    std::cout << "hitsc: aten login succeeded\n";
    std::cout << "hitsc: cookies stored: " << session.cookies.size() << '\n';

    if (options.fetch_bootstrap) {
        fetch_aten_ikvm_bootstrap(options.login, session.cookies);
    }

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

        const std::string cookie_header = session.cookies.header();
        if (!cookie_header.empty()) {
            request.set(http::field::cookie, cookie_header);
        }
    }));

    const std::string path = normalized_websocket_path(options.websocket_path);
    websocket::response_type response;
    ws.handshake(response, host, path);
    std::cout << "hitsc: aten websocket connected"
              << " path=" << path
              << " idle-timeout=" << options.idle_timeout_seconds << "s\n";

    beast::flat_buffer buffer;
    ws.read(buffer);
    const std::vector<std::uint8_t> bytes = buffer_bytes(buffer);
    std::cout << "hitsc: aten websocket message"
              << " bytes=" << bytes.size()
              << " binary=" << (ws.got_binary() ? "yes" : "no") << '\n';
    print_hex_preview(bytes);

    const std::string text = escaped_ascii(bytes);
    if (!text.empty()) {
        std::cout << "hitsc: aten websocket text=\"" << text << "\"\n";
    }

    beast::error_code error;
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(5));
    ws.close(websocket::close_code::normal, error);
    if (error) {
        std::cerr << "hitsc: aten websocket close warning: " << error.message() << '\n';
        error.clear();
        beast::get_lowest_layer(ws).socket().shutdown(tcp::socket::shutdown_both, error);
        error.clear();
        beast::get_lowest_layer(ws).socket().close(error);
    }
}

} // namespace hitsc
