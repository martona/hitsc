#include "http_client.hpp"

#include "app_info.hpp"
#include "log.hpp"
#include "text.hpp"
#include "tls.hpp"
#include "trace.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/version.hpp>
#include <zlib.h>

#include <chrono>
#include <stdexcept>
#include <utility>

namespace hitsc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

namespace {

struct ZStream {
    z_stream stream{};
    bool initialized = false;

    ~ZStream()
    {
        if (initialized) {
            inflateEnd(&stream);
        }
    }
};

bool is_gzip_encoded(const StringResponse& response)
{
    const auto encoding = response.find(http::field::content_encoding);
    if (encoding == response.end()) {
        return false;
    }

    const std::string normalized = lower_copy(trim_copy(encoding->value()));
    if (normalized == "identity") {
        return false;
    }
    if (normalized == "gzip" || normalized == "x-gzip") {
        return true;
    }

    throw std::runtime_error("unsupported Content-Encoding: " + std::string(encoding->value()));
}

std::runtime_error make_zlib_error(int code, const ZStream& state, std::string_view action)
{
    std::string message = "gzip decode failed while ";
    message += action;
    message += ": ";
    message += state.stream.msg != nullptr ? state.stream.msg : zError(code);
    return std::runtime_error(message);
}

std::string gzip_decode(std::string_view compressed)
{
    ZStream state;
    int result = inflateInit2(&state.stream, MAX_WBITS + 16);
    if (result != Z_OK) {
        throw make_zlib_error(result, state, "initializing");
    }
    state.initialized = true;

    state.stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
    state.stream.avail_in = static_cast<uInt>(compressed.size());

    std::string decoded;
    char output[16 * 1024];

    do {
        state.stream.next_out = reinterpret_cast<Bytef*>(output);
        state.stream.avail_out = sizeof(output);

        result = inflate(&state.stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            throw make_zlib_error(result, state, "inflating");
        }

        decoded.append(output, sizeof(output) - state.stream.avail_out);
    } while (result != Z_STREAM_END);

    return decoded;
}

void collect_cookies(const StringResponse& response, CookieJar& cookies)
{
    for (const auto& field : response) {
        if (field.name() == http::field::set_cookie) {
            cookies.add_set_cookie(field.value());
        }
    }
}

void set_request_headers(
    http::request<http::string_body>& request,
    const Url& url,
    const CookieJar* cookies,
    const std::vector<Header>& extra_headers)
{
    request.set(http::field::host, make_host_header(url));
    request.set(http::field::user_agent, std::string(kName) + "/" + std::string(BOOST_LIB_VERSION));
    request.set(http::field::accept, "application/json, text/plain, */*");
    request.set(http::field::accept_encoding, "gzip");

    if (cookies != nullptr) {
        const std::string cookie_header = cookies->header();
        if (!cookie_header.empty()) {
            request.set(http::field::cookie, cookie_header);
        }
    }

    for (const auto& header : extra_headers) {
        if (header.field != http::field::unknown) {
            request.set(header.field, header.value);
        } else {
            request.set(header.name, header.value);
        }
    }
}

std::string make_request_url(const Url& url, std::string_view target)
{
    std::string request_url = make_origin(url);
    if (target.empty()) {
        return request_url;
    }
    if (target.front() != '/' && target.front() != '?') {
        request_url += '/';
    } else if (target.front() == '?') {
        request_url += '/';
    }
    request_url += target;
    return request_url;
}

} // namespace

StringResponse https_request(
    const Url& url,
    bool insecure,
    http::verb method,
    std::string_view target,
    std::string body,
    std::string_view content_type,
    CookieJar* cookies,
    const std::vector<Header>& extra_headers,
    bool verbose,
    int timeout_seconds)
{
    asio::io_context io;
    ssl::context tls_context(ssl::context::tls_client);
    tcp::resolver resolver(io);
    beast::ssl_stream<beast::tcp_stream> stream(io, tls_context);

    configure_tls(tls_context, stream, url.host, insecure);
    set_server_name_indication(stream, url.host);

    const std::string request_url = make_request_url(url, target);
    const auto request_started_at = std::chrono::steady_clock::now();
    if (verbose) {
        log_info() << "https request starting"
                   << " base-url=" << make_origin(url)
                   << " target=" << target
                   << " url=" << request_url;
    }

    const auto endpoints = resolver.resolve(url.host, url.port);
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(timeout_seconds));
    beast::get_lowest_layer(stream).connect(endpoints);
    stream.handshake(ssl::stream_base::client);

    http::request<http::string_body> request{method, target, 11};
    request.body() = std::move(body);
    set_request_headers(request, url, cookies, extra_headers);
    if (!content_type.empty()) {
        request.set(http::field::content_type, content_type);
    }
    request.prepare_payload();

    log_http_request(request, verbose);
    http::write(stream, request);

    beast::flat_buffer buffer;
    StringResponse response;
    http::read(stream, buffer, response);

    if (cookies != nullptr) {
        collect_cookies(response, *cookies);
    }

    beast::error_code shutdown_error;
    stream.shutdown(shutdown_error);
    if (shutdown_error == asio::error::eof ||
        shutdown_error == ssl::error::stream_truncated) {
        shutdown_error = {};
    }
    if (shutdown_error) {
        throw beast::system_error(shutdown_error, "TLS shutdown failed");
    }

    if (verbose) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - request_started_at).count();
        log_info() << "https request finished"
                   << " duration-ms=" << elapsed_ms
                   << " url=" << request_url;
    }

    log_http_response(response, decode_response_body(response), verbose);
    return response;
}

std::string decode_response_body(const StringResponse& response)
{
    if (!is_gzip_encoded(response)) {
        return response.body();
    }

    return gzip_decode(response.body());
}

void require_success_status(const StringResponse& response, std::string_view context)
{
    if (response.result_int() >= 200 && response.result_int() < 300) {
        return;
    }

    const std::string body = decode_response_body(response);
    throw std::runtime_error(
        std::string(context) + " failed with HTTP " + std::to_string(response.result_int()) + ": " + body_snippet(body));
}

} // namespace hitsc
