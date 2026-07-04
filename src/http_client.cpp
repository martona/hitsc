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
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/version.hpp>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
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

} // namespace

void HttpCancelToken::cancel() noexcept
{
    std::lock_guard lock(mutex_);
    canceled_ = true;
    for (asio::io_context* io : contexts_) {
        io->stop();  // thread-safe; unblocks the request() pumping that context
    }
}

bool HttpCancelToken::canceled() const noexcept
{
    std::lock_guard lock(mutex_);
    return canceled_;
}

void HttpCancelToken::register_io(asio::io_context& io)
{
    std::lock_guard lock(mutex_);
    contexts_.push_back(&io);
    if (canceled_) {
        io.stop();  // cancel() already happened: make the first pump abort immediately
    }
}

void HttpCancelToken::unregister_io(asio::io_context& io) noexcept
{
    std::lock_guard lock(mutex_);
    contexts_.erase(std::remove(contexts_.begin(), contexts_.end(), &io), contexts_.end());
}

struct HttpsClient::Impl {
    using Stream = beast::ssl_stream<beast::tcp_stream>;

    // All I/O below is asynchronous, driven to completion by pumping io_ on the
    // calling thread. This is not a style choice: beast's timeouts only exist for
    // async operations ("Timeouts are not available when performing blocking calls"
    // -- basic_stream docs), and cancel()'s io_.stop() can only interrupt a run()
    // loop. The previous synchronous implementation had silently non-functional
    // timeouts and an inert cancel(), which let a hard-powered-off BMC hang every
    // teardown path.
    //
    // Every completion handler writes into shared_ptr-owned state (StepState /
    // Connection / Exchange) and never captures raw stack pointers, so a step
    // abandoned by cancel() can be drained -- or destroyed with io_ -- without
    // dangling.

    // One async step's outcome; handlers own it via shared_ptr.
    struct StepState {
        bool done = false;
        beast::error_code ec;
    };

    // One connection attempt's resources. The established stream_ aliases into
    // this, so handler-held copies keep everything alive together.
    struct Connection {
        Connection(asio::io_context& io, ssl::context& tls_context)
            : stream(io, tls_context)
            , timer(io)
        {
        }

        Stream stream;
        asio::steady_timer timer;  // resolve deadline (the resolver has no built-in one)
        tcp::resolver::results_type endpoints;
    };

    // One request/response exchange's buffers, alive until the handlers finish.
    struct Exchange {
        http::request<http::string_body> request;
        beast::flat_buffer buffer;
        StringResponse response;
    };

    // RAII binding of one request() to the cancel token, so token->cancel() from
    // another thread stops this client's io_context while the request runs.
    struct TokenRegistration {
        TokenRegistration(HttpCancelToken* token, asio::io_context& io)
            : token_(token)
            , io_(&io)
        {
            if (token_ != nullptr) {
                token_->register_io(*io_);
            }
        }

        ~TokenRegistration()
        {
            if (token_ != nullptr) {
                token_->unregister_io(*io_);
            }
        }

        TokenRegistration(const TokenRegistration&) = delete;
        TokenRegistration& operator=(const TokenRegistration&) = delete;

        HttpCancelToken* token_;
        asio::io_context* io_;
    };

    Impl(
        const Url& url,
        bool insecure,
        bool verbose,
        int timeout_seconds,
        TlsSessionCache* tls_session_cache,
        bool keep_alive,
        std::shared_ptr<HttpCancelToken> cancel_token)
        : url_(url)
        , insecure_(insecure)
        , verbose_(verbose)
        , timeout_seconds_(timeout_seconds)
        , tls_session_cache_(tls_session_cache)
        , keep_alive_(keep_alive)
        , cancel_token_(std::move(cancel_token))
        , tls_context_(ssl::context::tls_client)
        , resolver_(io_)
    {
        if (tls_session_cache_ != nullptr) {
            configure_tls_session_cache(tls_context_, *tls_session_cache_);
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
        // A client-level cancel() is not sticky (the next request reconnects); a
        // token cancel is, and begin_step() enforces it before every operation.
        canceled_.store(false);
        TokenRegistration token_registration(cancel_token_.get(), io_);

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
                const boost::system::error_code code = ex.code();
                // Never retry a canceled request (teardown wants out NOW) or one that
                // hit its deadline (the peer is unresponsive; a second full wait helps
                // nobody and doubles a hang).
                if (code == asio::error::operation_aborted || code == beast::error::timeout) {
                    throw;
                }
                // A reused keep-alive connection that the server closed while idle
                // surfaces as EOF before any response byte (end_of_stream). The request
                // was never processed, so retrying once is safe even for non-idempotent
                // methods (e.g. the power POSTs) -- this is the standard stale-keepalive
                // recovery. Other (mid-exchange) errors stay gated on idempotency.
                const bool connection_closed =
                    code == http::error::end_of_stream ||
                    code == ssl::error::stream_truncated ||
                    code == boost::asio::error::eof ||
                    code == boost::asio::error::connection_reset ||
                    code == boost::asio::error::broken_pipe;
                if (reused_connection && !retried &&
                    (can_retry_reused_request(method) || connection_closed)) {
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

    void cancel() noexcept
    {
        // Thread-safe (io_context::stop may be called from any thread). request()
        // pumps every async operation through io_.run(), so the stop returns run()
        // before the step's handler fires; finish_step() sees the incomplete step,
        // aborts the in-flight operation, and throws operation_aborted. Not sticky:
        // request() clears the flag on entry and restarts io_ before each step.
        canceled_.store(true);
        io_.stop();
    }

    void set_timeout_seconds(int timeout_seconds) noexcept
    {
        timeout_seconds_ = timeout_seconds;
    }

private:
    // Throws if this request was canceled, then readies io_ for the next step.
    // (io_.restart() clears a stop flag, so the cancel checks must come first; a
    // cancel landing in the hair's width between check and run() still gets caught
    // by the step's own deadline -- bounded, never a hang.)
    void begin_step(const char* what)
    {
        if (canceled_.load() || (cancel_token_ != nullptr && cancel_token_->canceled())) {
            throw boost_system::system_error(asio::error::operation_aborted, what);
        }
        io_.restart();
    }

    // Pump io_ until the step completes. If run() was stopped first (cancel()),
    // abort the in-flight operation and drain its handlers -- they own their state
    // via shared_ptr, so even a drain truncated by a second stop dangles nothing --
    // then surface the abort.
    void finish_step(
        const std::shared_ptr<StepState>& step,
        const char* what,
        const std::function<void()>& abort_in_flight)
    {
        io_.run();
        if (!step->done) {
            abort_in_flight();
            io_.restart();
            io_.run();
            throw boost_system::system_error(asio::error::operation_aborted, what);
        }
        if (step->ec) {
            throw boost_system::system_error(step->ec, what);
        }
    }

    static void close_stream(Stream& stream) noexcept
    {
        beast::error_code ignored;
        beast::get_lowest_layer(stream).socket().close(ignored);
    }

    void connect()
    {
        auto connection = std::make_shared<Connection>(io_, tls_context_);
        configure_tls(tls_context_, connection->stream, url_.host, insecure_);
        set_server_name_indication(connection->stream, url_.host);
        if (tls_session_cache_ != nullptr) {
            prepare_tls_session_resumption(connection->stream, *tls_session_cache_, verbose_);
        }

        {
            begin_step("resolve host");
            auto step = std::make_shared<StepState>();
            connection->timer.expires_after(std::chrono::seconds(timeout_seconds_));
            connection->timer.async_wait([this, step](beast::error_code timer_error) {
                if (!timer_error && !step->done) {
                    resolver_.cancel();
                }
            });
            resolver_.async_resolve(
                url_.host, url_.port,
                [connection, step](beast::error_code error, tcp::resolver::results_type results) {
                    connection->endpoints = std::move(results);
                    step->ec = error;
                    step->done = true;
                    connection->timer.cancel();
                });
            finish_step(step, "resolve host", [this, connection] {
                resolver_.cancel();
                connection->timer.cancel();
            });
        }
        {
            begin_step("connect");
            auto step = std::make_shared<StepState>();
            beast::get_lowest_layer(connection->stream)
                .expires_after(std::chrono::seconds(timeout_seconds_));
            beast::get_lowest_layer(connection->stream)
                .async_connect(
                    connection->endpoints,
                    [connection, step](beast::error_code error, const tcp::endpoint&) {
                        step->ec = error;
                        step->done = true;
                    });
            finish_step(step, "connect", [connection] { close_stream(connection->stream); });
        }
        {
            begin_step("tls handshake");
            auto step = std::make_shared<StepState>();
            beast::get_lowest_layer(connection->stream)
                .expires_after(std::chrono::seconds(timeout_seconds_));
            connection->stream.async_handshake(
                ssl::stream_base::client,
                [connection, step](beast::error_code error) {
                    step->ec = error;
                    step->done = true;
                });
            finish_step(step, "tls handshake", [connection] { close_stream(connection->stream); });
        }

        if (tls_session_cache_ != nullptr) {
            log_tls_session_handshake_result(connection->stream, *tls_session_cache_, verbose_);
        }

        // Alias into the Connection so handler-held copies and stream_ share one lifetime.
        stream_ = std::shared_ptr<Stream>(connection, &connection->stream);
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

        auto stream = stream_;  // handlers keep the connection alive if abandoned
        auto exchange = std::make_shared<Exchange>();
        exchange->request =
            make_http_request(method, target, body, content_type, cookies, extra_headers);
        log_http_request(exchange->request, verbose_);

        {
            begin_step("send request");
            auto step = std::make_shared<StepState>();
            beast::get_lowest_layer(*stream).expires_after(std::chrono::seconds(timeout_seconds_));
            http::async_write(
                *stream, exchange->request,
                [stream, exchange, step](beast::error_code error, std::size_t) {
                    step->ec = error;
                    step->done = true;
                });
            finish_step(step, "send request", [stream] { close_stream(*stream); });
        }
        {
            begin_step("read response");
            auto step = std::make_shared<StepState>();
            beast::get_lowest_layer(*stream).expires_after(std::chrono::seconds(timeout_seconds_));
            http::async_read(
                *stream, exchange->buffer, exchange->response,
                [stream, exchange, step](beast::error_code error, std::size_t) {
                    step->ec = error;
                    step->done = true;
                });
            finish_step(step, "read response", [stream] { close_stream(*stream); });
        }

        if (cookies != nullptr) {
            collect_cookies(exchange->response, *cookies);
        }

        return std::move(exchange->response);
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
            // Graceful close_notify, but tightly bounded: asio's TLS shutdown WAITS
            // to read the peer's close_notify, which never arrives from a dead BMC.
            // This runs on teardown paths and is pure courtesy -- 2s, then hard close.
            begin_step("tls shutdown");
            auto stream = stream_;
            auto step = std::make_shared<StepState>();
            beast::get_lowest_layer(*stream).expires_after(std::chrono::seconds(2));
            stream->async_shutdown([stream, step](beast::error_code error) {
                step->ec = error;
                step->done = true;
            });
            io_.run();
            if (!step->done) {
                close_stream(*stream);
                io_.restart();
                io_.run();
            }
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
    std::shared_ptr<HttpCancelToken> cancel_token_;
    std::atomic_bool canceled_{false};
    asio::io_context io_;
    ssl::context tls_context_;
    tcp::resolver resolver_;
    std::shared_ptr<Stream> stream_;  // aliases into a Connection (see connect())
};

HttpsClient::HttpsClient(
    const Url& url,
    bool insecure,
    bool verbose,
    int timeout_seconds,
    TlsSessionCache* tls_session_cache,
    bool keep_alive,
    std::shared_ptr<HttpCancelToken> cancel_token)
    : impl_(std::make_unique<Impl>(
          url, insecure, verbose, timeout_seconds, tls_session_cache, keep_alive,
          std::move(cancel_token)))
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

void HttpsClient::cancel() noexcept
{
    if (impl_) {
        impl_->cancel();
    }
}

void HttpsClient::set_timeout_seconds(int timeout_seconds) noexcept
{
    if (impl_) {
        impl_->set_timeout_seconds(timeout_seconds);
    }
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
