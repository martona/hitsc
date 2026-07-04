#include "bmc_session.hpp"

#include "app_info.hpp"
#include "errors.hpp"
#include "log.hpp"
#include "text.hpp"
#include "tls.hpp"
#include "url.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/system/system_error.hpp>
#include <boost/version.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hitsc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
namespace websocket = boost::beast::websocket;
using tcp = asio::ip::tcp;

namespace {

std::string base64_encode(std::string_view input)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const auto a = static_cast<std::uint32_t>(static_cast<unsigned char>(input[i]));
        const auto b = i + 1 < input.size()
            ? static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1]))
            : 0;
        const auto c = i + 2 < input.size()
            ? static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 2]))
            : 0;
        const std::uint32_t triple = (a << 16) | (b << 8) | c;

        encoded.push_back(alphabet[(triple >> 18) & 0x3f]);
        encoded.push_back(alphabet[(triple >> 12) & 0x3f]);
        encoded.push_back(i + 1 < input.size() ? alphabet[(triple >> 6) & 0x3f] : '=');
        encoded.push_back(i + 2 < input.size() ? alphabet[triple & 0x3f] : '=');
    }
    return encoded;
}

std::string field_source_value(const LoginOptions& options, const BmcLoginField& field)
{
    switch (field.source) {
    case BmcCredentialSource::Username:
        return options.username;
    case BmcCredentialSource::Password:
        return options.password;
    case BmcCredentialSource::Literal:
        return field.literal;
    }

    return {};
}

std::string transform_value(std::string value, BmcCredentialTransform transform)
{
    switch (transform) {
    case BmcCredentialTransform::None:
        return value;
    case BmcCredentialTransform::Base64:
        return base64_encode(value);
    }

    return value;
}

std::string build_form_body(const LoginOptions& options, const BmcLoginProfile& profile)
{
    std::string body;
    for (const BmcLoginField& field : profile.fields) {
        if (!body.empty()) {
            body += '&';
        }

        std::string value = field_source_value(options, field);
        value = transform_value(std::move(value), field.transform);
        body += field.encode_name ? form_url_encode(field.name) : field.name;
        body += '=';
        body += form_url_encode(value);
    }
    return body;
}

void set_extra_headers(websocket::request_type& request, const std::vector<Header>& extra_headers)
{
    for (const Header& header : extra_headers) {
        if (header.field != http::field::unknown) {
            request.set(header.field, header.value);
        } else {
            request.set(header.name, header.value);
        }
    }
}

std::string websocket_handshake_error_message(
    const BmcWebSocketConnectOptions& options,
    const boost::system::system_error& error,
    const websocket::response_type& response)
{
    std::ostringstream message;
    message << options.log_name << " connection failed"
            << " path=" << options.path
            << ": " << error.code().message();
    if (error.code() == websocket::error::upgrade_declined) {
        message << " HTTP " << response.result_int();
        if (!response.reason().empty()) {
            message << " " << response.reason();
        }
        const std::string& body = response.body();
        if (!body.empty()) {
            message << ": " << body_snippet(body);
        }
    }
    return message.str();
}

} // namespace

BmcWebSocketConnection::BmcWebSocketConnection(std::string role)
    : tls_context_(ssl::context::tls_client)
    , stream_(std::make_shared<BmcWebSocketStream>(io_, tls_context_))
    , role_(std::move(role))
{
}

BmcWebSocketConnection::~BmcWebSocketConnection()
{
    force_close();
}

asio::io_context& BmcWebSocketConnection::io_context()
{
    return io_;
}

std::shared_ptr<BmcWebSocketStream> BmcWebSocketConnection::stream()
{
    return stream_;
}

const std::string& BmcWebSocketConnection::role() const
{
    return role_;
}

void BmcWebSocketConnection::force_close() noexcept
{
    closed_.store(true);
    // Unconditionally break whatever is pumping io_: an in-progress open_websocket
    // (its pump sees the stop and unwinds) or a live session's io.run(). Socket
    // close alone is not enough for the open phase -- before async_connect starts
    // there is no pending operation for it to abort.
    io_.stop();
    if (opening_.load()) {
        // The opening thread owns the socket and closes it on unwind; touching it
        // from this thread would race its handlers.
        return;
    }
    if (!stream_) {
        return;
    }

    beast::error_code error;
    beast::get_lowest_layer(*stream_).socket().cancel(error);
    error.clear();
    beast::get_lowest_layer(*stream_).socket().shutdown(tcp::socket::shutdown_both, error);
    error.clear();
    beast::get_lowest_layer(*stream_).socket().close(error);
}

struct BmcWebSession::WebSocketRegistry {
    mutable std::mutex mutex;
    std::vector<BmcWebSocketConnectionPtr> connections;
};

BmcWebSession::BmcWebSession(const LoginOptions& options)
    : base_url_(options.base_url)
    , insecure_(options.insecure)
    , verbose_(options.verbose)
    , tls_session_cache_(
          options.tls_session_cache ? options.tls_session_cache : std::make_shared<TlsSessionCache>(16))
    , client_(
          options.base_url,
          options.insecure,
          options.verbose,
          30,
          tls_session_cache_.get(),
          !options.debug_disable_http_keepalive,
          options.cancel_token)
    , websockets_(std::make_unique<WebSocketRegistry>())
{
}

BmcWebSession::~BmcWebSession()
{
    close_all_websockets();
}

StringResponse BmcWebSession::request(
    http::verb method,
    std::string_view target,
    std::string body,
    std::string_view content_type,
    const std::vector<Header>& extra_headers)
{
    std::vector<Header> headers;
    headers.reserve(2 + extra_headers.size());

    const std::string origin = make_origin(base_url_);
    headers.push_back(Header{http::field::origin, {}, origin});
    headers.push_back(Header{http::field::referer, {}, origin + "/"});
    headers.insert(headers.end(), extra_headers.begin(), extra_headers.end());

    return client_.request(
        method,
        target,
        std::move(body),
        content_type,
        &cookies_,
        headers);
}

void BmcWebSession::cancel_in_flight_request() noexcept
{
    // Thread-safe; aborts a blocking request() running on another thread (the power
    // worker) so teardown does not wait on a hung POST.
    client_.cancel();
}

void BmcWebSession::set_http_timeout_seconds(int seconds) noexcept
{
    client_.set_timeout_seconds(seconds);
}

BmcWebSocketOpenResult BmcWebSession::open_websocket(BmcWebSocketConnectOptions options)
{
    auto connection = BmcWebSocketConnectionPtr(new BmcWebSocketConnection(options.role));
    configure_tls_session_cache(connection->tls_context_, *tls_session_cache_);
    BmcWebSocketStream& ws = *connection->stream_;

    const std::string host = make_host_header(base_url_);
    const std::string origin = make_origin(base_url_);
    const auto websocket_started_at = std::chrono::steady_clock::now();
    if (verbose_) {
        log_info() << options.log_name << " connecting"
                   << " path=" << options.path
                   << " url=wss://" << host << options.path
                   << " idle-timeout=" << (options.idle_timeout_seconds > 0 ? std::to_string(options.idle_timeout_seconds) + "s" : "disabled");
    }

    // Register BEFORE any network step so force_close_websocket(role) can abort an
    // in-progress open: force_close() stops the io_context this open pumps, and the
    // pump below notices and unwinds. Deregistered again on failure.
    if (websockets_ != nullptr) {
        std::lock_guard lock(websockets_->mutex);
        websockets_->connections.push_back(connection);
    }
    connection->opening_.store(true);
    const auto unregister_connection = [this, &connection]() noexcept {
        if (websockets_ == nullptr) {
            return;
        }
        std::lock_guard lock(websockets_->mutex);
        auto& list = websockets_->connections;
        list.erase(std::remove(list.begin(), list.end(), connection), list.end());
    };

    // The open runs as pumped ASYNC steps: beast timeouts only apply to async
    // operations, and only a stoppable run() loop makes the open force-closeable.
    // Handlers own their state via shared_ptr (never stack references), so a step
    // abandoned by a force-close drains or dies with the connection's io_context
    // without dangling.
    struct OpenStep {
        bool done = false;
        beast::error_code ec;
    };
    struct OpenState {
        explicit OpenState(asio::io_context& io)
            : resolver(io)
            , timer(io)
        {
        }

        tcp::resolver resolver;
        asio::steady_timer timer;  // resolve deadline (the resolver has no built-in one)
        tcp::resolver::results_type endpoints;
        websocket::response_type response;
    };

    asio::io_context& io = connection->io_;
    auto state = std::make_shared<OpenState>(io);
    auto stream = connection->stream_;

    const auto begin_step = [&io, &connection](const char* what) {
        if (connection->closed_.load()) {
            throw boost::system::system_error(asio::error::operation_aborted, what);
        }
        io.restart();
    };
    const auto finish_step = [&io](
                                 const std::shared_ptr<OpenStep>& step, const char* what,
                                 const std::function<void()>& abort_in_flight) {
        io.run();
        if (!step->done) {
            // run() was stopped by force_close() mid-step: abort the in-flight
            // operation, drain its handlers, and unwind.
            abort_in_flight();
            io.restart();
            io.run();
            throw boost::system::system_error(asio::error::operation_aborted, what);
        }
        if (step->ec) {
            throw boost::system::system_error(step->ec, what);
        }
    };
    const auto close_socket = [stream] {
        beast::error_code ignored;
        beast::get_lowest_layer(*stream).socket().close(ignored);
    };

    try {
        configure_tls(connection->tls_context_, ws.next_layer(), base_url_.host, insecure_);
        set_server_name_indication(ws.next_layer(), base_url_.host);
        prepare_tls_session_resumption(ws.next_layer(), *tls_session_cache_, verbose_);

        {
            begin_step("websocket resolve");
            auto step = std::make_shared<OpenStep>();
            state->timer.expires_after(std::chrono::seconds(30));
            state->timer.async_wait([state, step](beast::error_code timer_error) {
                if (!timer_error && !step->done) {
                    state->resolver.cancel();
                }
            });
            state->resolver.async_resolve(
                base_url_.host, base_url_.port,
                [state, step](beast::error_code error, tcp::resolver::results_type results) {
                    state->endpoints = std::move(results);
                    step->ec = error;
                    step->done = true;
                    state->timer.cancel();
                });
            finish_step(step, "websocket resolve", [state] {
                state->resolver.cancel();
                state->timer.cancel();
            });
        }
        {
            begin_step("websocket connect");
            auto step = std::make_shared<OpenStep>();
            beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(30));
            beast::get_lowest_layer(ws).async_connect(
                state->endpoints,
                [stream, state, step](beast::error_code error, const tcp::endpoint&) {
                    step->ec = error;
                    step->done = true;
                });
            finish_step(step, "websocket connect", close_socket);
        }
        if (options.tcp_no_delay) {
            beast::get_lowest_layer(ws).socket().set_option(tcp::no_delay(true));
        }
        {
            begin_step("websocket tls handshake");
            auto step = std::make_shared<OpenStep>();
            beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(30));
            ws.next_layer().async_handshake(
                ssl::stream_base::client,
                [stream, step](beast::error_code error) {
                    step->ec = error;
                    step->done = true;
                });
            finish_step(step, "websocket tls handshake", close_socket);
        }
        log_tls_session_handshake_result(ws.next_layer(), *tls_session_cache_, verbose_);
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

        ws.set_option(websocket::stream_base::decorator(
            [this, origin, extra_headers = options.extra_headers](websocket::request_type& request) {
                request.set(http::field::user_agent, std::string(kName) + "/" + std::string(BOOST_LIB_VERSION));
                request.set(http::field::origin, origin);
                set_extra_headers(request, extra_headers);

                const std::string cookie_header = cookies_.header();
                if (!cookie_header.empty()) {
                    request.set(http::field::cookie, cookie_header);
                }
            }));

        {
            begin_step("websocket handshake");
            auto step = std::make_shared<OpenStep>();
            ws.async_handshake(
                state->response, host, options.path,
                [stream, state, step](beast::error_code error) {
                    step->ec = error;
                    step->done = true;
                });
            finish_step(step, "websocket handshake", close_socket);
        }

        connection->opening_.store(false);
        io.restart();  // leave the io_context runnable for the session's own io.run()
        // A force-close that raced the open wins: hand back nothing. Checked AFTER
        // the opening_ flip and restart so no ordering loses the close -- from here
        // on force_close() also closes the socket itself.
        if (connection->closed_.load()) {
            throw boost::system::system_error(asio::error::operation_aborted, "websocket open");
        }
    } catch (const boost::system::system_error& ex) {
        unregister_connection();
        connection->opening_.store(false);
        connection->force_close();
        throw UserError(websocket_handshake_error_message(options, ex, state->response));
    } catch (...) {
        unregister_connection();
        connection->opening_.store(false);
        connection->force_close();
        throw;
    }

    const auto websocket_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - websocket_started_at).count();
    {
        LogLine line = log_info();
        line << options.log_name << " connected"
             << " path=" << options.path
             << " url=wss://" << host << options.path
             << " duration-ms=" << websocket_elapsed_ms;
        if (options.idle_timeout_seconds > 0) {
            line << " idle-timeout=" << options.idle_timeout_seconds << "s";
        } else {
            line << " idle-timeout=disabled";
        }
    }

    return BmcWebSocketOpenResult{std::move(connection), std::move(state->response)};
}

void BmcWebSession::force_close_websocket(std::string_view role) noexcept
{
    if (websockets_ == nullptr) {
        return;
    }

    std::vector<BmcWebSocketConnectionPtr> connections;
    {
        std::lock_guard lock(websockets_->mutex);
        for (const auto& connection : websockets_->connections) {
            if (connection && std::string_view(connection->role()) == role) {
                connections.push_back(connection);
            }
        }
    }

    for (const auto& connection : connections) {
        connection->force_close();
    }
}

void BmcWebSession::close_all_websockets() noexcept
{
    if (websockets_ == nullptr) {
        return;
    }

    std::vector<BmcWebSocketConnectionPtr> connections;
    {
        std::lock_guard lock(websockets_->mutex);
        connections.swap(websockets_->connections);
    }

    for (const auto& connection : connections) {
        connection->force_close();
    }
}

std::size_t BmcWebSession::cookie_count() const
{
    return cookies_.size();
}

std::string_view BmcWebSession::session_token() const
{
    return session_token_;
}

void BmcWebSession::set_cookie(std::string name, std::string value)
{
    cookies_.set(std::move(name), std::move(value));
}

void BmcWebSession::set_session_token(std::string token, std::string_view token_cookie_name)
{
    session_token_ = std::move(token);
    if (!session_token_.empty() && !token_cookie_name.empty()) {
        cookies_.set(std::string(token_cookie_name), session_token_);
    }
}

BmcWebSession login_bmc_web_session(const LoginOptions& options, const BmcLoginProfile& profile)
{
    BmcWebSession session(options);
    auto response = session.request(
        http::verb::post,
        profile.login_target,
        build_form_body(options, profile),
        profile.content_type);

    const int status = static_cast<int>(response.result_int());
    if (status < 200 || status > profile.max_success_status) {
        const std::string body = decode_response_body(response);
        throw UserError(
            profile.vendor_name + " login failed with HTTP "
            + std::to_string(status) + ": " + body_snippet(body));
    }

    if (profile.require_login_cookie && session.cookie_count() == 0) {
        throw UserError(
            profile.vendor_name
            + " login failed: authentication response did not set a session cookie");
    }

    const std::string decoded_body = decode_response_body(response);
    if (profile.token_parser != nullptr) {
        session.set_session_token(profile.token_parser(decoded_body), profile.token_cookie_name);
    }

    return session;
}

} // namespace hitsc
