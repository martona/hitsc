#include "bmc_session.hpp"

#include "app_info.hpp"
#include "errors.hpp"
#include "log.hpp"
#include "text.hpp"
#include "tls.hpp"
#include "url.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/system/system_error.hpp>
#include <boost/version.hpp>

#include <algorithm>
#include <chrono>
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
    , tls_session_cache_(std::make_unique<TlsSessionCache>(16))
    , client_(
          options.base_url,
          options.insecure,
          options.verbose,
          30,
          tls_session_cache_.get(),
          !options.debug_disable_http_keepalive)
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

BmcWebSocketOpenResult BmcWebSession::open_websocket(BmcWebSocketConnectOptions options)
{
    auto connection = BmcWebSocketConnectionPtr(new BmcWebSocketConnection(options.role));
    configure_tls_session_cache(connection->tls_context_, *tls_session_cache_);
    BmcWebSocketStream& ws = *connection->stream_;

    websocket::response_type response;
    const std::string host = make_host_header(base_url_);
    const std::string origin = make_origin(base_url_);
    const auto websocket_started_at = std::chrono::steady_clock::now();
    if (verbose_) {
        log_info() << options.log_name << " connecting"
                   << " path=" << options.path
                   << " url=wss://" << host << options.path
                   << " idle-timeout=" << (options.idle_timeout_seconds > 0 ? std::to_string(options.idle_timeout_seconds) + "s" : "disabled");
    }

    try {
        configure_tls(connection->tls_context_, ws.next_layer(), base_url_.host, insecure_);
        set_server_name_indication(ws.next_layer(), base_url_.host);
        prepare_tls_session_resumption(ws.next_layer(), *tls_session_cache_, verbose_);

        tcp::resolver resolver(connection->io_);
        const auto endpoints = resolver.resolve(base_url_.host, base_url_.port);
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(30));
        beast::get_lowest_layer(ws).connect(endpoints);
        if (options.tcp_no_delay) {
            beast::get_lowest_layer(ws).socket().set_option(tcp::no_delay(true));
        }
        ws.next_layer().handshake(ssl::stream_base::client);
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

        ws.handshake(response, host, options.path);
    } catch (const boost::system::system_error& ex) {
        connection->force_close();
        throw UserError(websocket_handshake_error_message(options, ex, response));
    } catch (...) {
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

    if (websockets_ != nullptr) {
        std::lock_guard lock(websockets_->mutex);
        websockets_->connections.push_back(connection);
    }

    return BmcWebSocketOpenResult{std::move(connection), std::move(response)};
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

    const std::string decoded_body = decode_response_body(response);
    if (profile.token_parser != nullptr) {
        session.set_session_token(profile.token_parser(decoded_body), profile.token_cookie_name);
    }

    return session;
}

} // namespace hitsc
