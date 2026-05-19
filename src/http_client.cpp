#include "http_client.hpp"

#include "app_info.hpp"
#include "log.hpp"
#include "text.hpp"
#include "tls.hpp"
#include "tls_session_cache.hpp"
#include "trace.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/version.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <zlib.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <utility>

namespace hitsc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace boost_system = boost::system;
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

bool can_retry_reused_request(http::verb method)
{
    switch (method) {
    case http::verb::get:
    case http::verb::head:
    case http::verb::options:
    case http::verb::trace:
        return true;
    default:
        return false;
    }
}

int tls_session_cache_ex_data_index()
{
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int cache_new_tls_session(SSL* ssl_handle, SSL_SESSION* session)
{
    const int index = tls_session_cache_ex_data_index();
    if (index < 0) {
        return 0;
    }

    auto* cache = static_cast<TlsSessionCache*>(SSL_get_ex_data(ssl_handle, index));
    if (cache == nullptr) {
        return 0;
    }

    return cache->push(session) ? 1 : 0;
}

} // namespace

struct HttpsClient::Impl {
    using Stream = beast::ssl_stream<beast::tcp_stream>;

    Impl(
        const Url& url,
        bool insecure,
        bool verbose,
        int timeout_seconds,
        TlsSessionCache* tls_session_cache,
        bool keep_alive)
        : url_(url)
        , insecure_(insecure)
        , verbose_(verbose)
        , timeout_seconds_(timeout_seconds)
        , tls_session_cache_(tls_session_cache)
        , keep_alive_(keep_alive)
        , tls_context_(ssl::context::tls_client)
        , resolver_(io_)
    {
        if (tls_session_cache_ != nullptr) {
            const int index = tls_session_cache_ex_data_index();
            if (index < 0) {
                throw std::runtime_error("failed to allocate TLS session cache context index");
            }

            SSL_CTX_set_session_cache_mode(
                tls_context_.native_handle(),
                SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
            SSL_CTX_sess_set_new_cb(tls_context_.native_handle(), &cache_new_tls_session);
        }
    }

    ~Impl()
    {
        shutdown_connection();
    }

    StringResponse request(
        http::verb method,
        std::string_view target,
        const std::string& body,
        std::string_view content_type,
        CookieJar* cookies,
        const std::vector<Header>& extra_headers)
    {
        const std::string request_url = make_request_url(url_, target);
        const auto request_started_at = std::chrono::steady_clock::now();
        if (verbose_) {
            log_info() << "https request starting"
                       << " base-url=" << make_origin(url_)
                       << " target=" << target
                       << " url=" << request_url;
        }

        bool retried = false;
        while (true) {
            const bool reused_connection = stream_ != nullptr;

            try {
                StringResponse response = send_request(method, target, body, content_type, cookies, extra_headers);
                const bool reusable = keep_alive_ && response.keep_alive();

                log_request_finished(request_started_at, request_url, reused_connection);
                try {
                    log_http_response(response, decode_response_body(response), verbose_);
                } catch (...) {
                    if (!reusable) {
                        shutdown_connection();
                    }
                    throw;
                }

                if (!reusable) {
                    if (verbose_) {
                        log_debug() << "https connection closing"
                                    << " reason=" << (!keep_alive_ ? "client-keepalive-disabled" : "server-not-keepalive")
                                    << " response-keepalive=" << (response.keep_alive() ? "yes" : "no")
                                    << " url=" << request_url;
                    }
                    shutdown_connection();
                }

                return response;
            } catch (const boost_system::system_error& ex) {
                close_connection();
                if (reused_connection && !retried && can_retry_reused_request(method)) {
                    retried = true;
                    if (verbose_) {
                        log_debug() << "https existing connection failed; reconnecting once"
                                    << " url=" << request_url
                                    << " error=" << ex.what();
                    }
                    continue;
                }
                throw;
            } catch (...) {
                close_connection();
                throw;
            }
        }
    }

private:
    void connect()
    {
        io_.restart();

        auto next_stream = std::make_unique<Stream>(io_, tls_context_);
        configure_tls(tls_context_, *next_stream, url_.host, insecure_);
        set_server_name_indication(*next_stream, url_.host);
        configure_tls_session_resumption(*next_stream);

        const auto endpoints = resolver_.resolve(url_.host, url_.port);
        beast::get_lowest_layer(*next_stream).expires_after(std::chrono::seconds(timeout_seconds_));
        beast::get_lowest_layer(*next_stream).connect(endpoints);
        next_stream->handshake(ssl::stream_base::client);

        if (verbose_ && tls_session_cache_ != nullptr) {
            log_debug() << "tls session handshake"
                        << " reused=" << (SSL_session_reused(next_stream->native_handle()) == 1 ? "yes" : "no")
                        << " cached-sessions=" << tls_session_cache_->size();
        }

        stream_ = std::move(next_stream);
    }

    void configure_tls_session_resumption(Stream& stream)
    {
        if (tls_session_cache_ == nullptr) {
            return;
        }

        SSL* ssl_handle = stream.native_handle();
        if (SSL_set_ex_data(ssl_handle, tls_session_cache_ex_data_index(), tls_session_cache_) != 1) {
            throw std::runtime_error("failed to attach TLS session cache to connection");
        }

        SslSessionPtr session = tls_session_cache_->pop();
        if (session == nullptr) {
            return;
        }

        if (SSL_set_session(ssl_handle, session.get()) != 1) {
            ERR_clear_error();
            if (verbose_) {
                log_debug() << "tls session resumption ticket rejected before handshake";
            }
            return;
        }

        if (verbose_) {
            log_debug() << "tls session resumption ticket offered";
        }
    }

    http::request<http::string_body> make_http_request(
        http::verb method,
        std::string_view target,
        const std::string& body,
        std::string_view content_type,
        CookieJar* cookies,
        const std::vector<Header>& extra_headers) const
    {
        http::request<http::string_body> request{method, target, 11};
        request.body() = body;
        set_request_headers(request, url_, cookies, extra_headers);
        request.keep_alive(keep_alive_);
        if (!content_type.empty()) {
            request.set(http::field::content_type, content_type);
        }
        request.prepare_payload();
        return request;
    }

    StringResponse send_request(
        http::verb method,
        std::string_view target,
        const std::string& body,
        std::string_view content_type,
        CookieJar* cookies,
        const std::vector<Header>& extra_headers)
    {
        if (stream_ == nullptr) {
            connect();
        }

        auto request = make_http_request(method, target, body, content_type, cookies, extra_headers);

        log_http_request(request, verbose_);
        beast::get_lowest_layer(*stream_).expires_after(std::chrono::seconds(timeout_seconds_));
        http::write(*stream_, request);

        beast::flat_buffer buffer;
        StringResponse response;
        beast::get_lowest_layer(*stream_).expires_after(std::chrono::seconds(timeout_seconds_));
        http::read(*stream_, buffer, response);

        if (cookies != nullptr) {
            collect_cookies(response, *cookies);
        }

        return response;
    }

    void log_request_finished(
        std::chrono::steady_clock::time_point request_started_at,
        const std::string& request_url,
        bool reused_connection) const
    {
        if (!verbose_) {
            return;
        }

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - request_started_at).count();
        log_info() << "https request finished"
                   << " duration-ms=" << elapsed_ms
                   << " connection-reused=" << (reused_connection ? "yes" : "no")
                   << " url=" << request_url;
    }

    void shutdown_connection() noexcept
    {
        if (stream_ == nullptr) {
            return;
        }

        try {
            beast::get_lowest_layer(*stream_).expires_after(std::chrono::seconds(timeout_seconds_));
            beast::error_code shutdown_error;
            stream_->shutdown(shutdown_error);
        } catch (...) {
        }

        close_connection();
    }

    void close_connection() noexcept
    {
        if (stream_ == nullptr) {
            return;
        }

        beast::error_code ignored;
        beast::get_lowest_layer(*stream_).socket().shutdown(tcp::socket::shutdown_both, ignored);
        beast::get_lowest_layer(*stream_).socket().close(ignored);
        stream_.reset();
    }

    Url url_;
    bool insecure_ = false;
    bool verbose_ = false;
    int timeout_seconds_ = 30;
    TlsSessionCache* tls_session_cache_ = nullptr;
    bool keep_alive_ = true;
    asio::io_context io_;
    ssl::context tls_context_;
    tcp::resolver resolver_;
    std::unique_ptr<Stream> stream_;
};

HttpsClient::HttpsClient(
    const Url& url,
    bool insecure,
    bool verbose,
    int timeout_seconds,
    TlsSessionCache* tls_session_cache,
    bool keep_alive)
    : impl_(std::make_unique<Impl>(url, insecure, verbose, timeout_seconds, tls_session_cache, keep_alive))
{
}

HttpsClient::~HttpsClient() = default;

HttpsClient::HttpsClient(HttpsClient&&) noexcept = default;

HttpsClient& HttpsClient::operator=(HttpsClient&&) noexcept = default;

StringResponse HttpsClient::request(
    http::verb method,
    std::string_view target,
    std::string body,
    std::string_view content_type,
    CookieJar* cookies,
    const std::vector<Header>& extra_headers)
{
    return impl_->request(method, target, body, content_type, cookies, extra_headers);
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
