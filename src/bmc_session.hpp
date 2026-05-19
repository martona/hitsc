#pragma once

#include "http_client.hpp"
#include "options.hpp"
#include "tls_session_cache.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hitsc {

enum class BmcCredentialSource {
    Username,
    Password,
    Literal,
};

enum class BmcCredentialTransform {
    None,
    Base64,
};

struct BmcLoginField {
    std::string name;
    BmcCredentialSource source = BmcCredentialSource::Literal;
    std::string literal;
    BmcCredentialTransform transform = BmcCredentialTransform::None;
    bool encode_name = true;
};

using BmcSessionTokenParser = std::string (*)(std::string_view body);

struct BmcLoginProfile {
    std::string vendor_name;
    std::string login_target;
    std::vector<BmcLoginField> fields;
    std::string content_type = "application/x-www-form-urlencoded";
    BmcSessionTokenParser token_parser = nullptr;
    std::string token_cookie_name;
    int max_success_status = 299;
};

using BmcWebSocketStream = boost::beast::websocket::stream<
    boost::beast::ssl_stream<boost::beast::tcp_stream>>;

struct BmcWebSocketConnectOptions {
    std::string role;
    std::string log_name = "websocket";
    std::string path;
    int idle_timeout_seconds = 0;
    bool tcp_no_delay = false;
    std::vector<Header> extra_headers;
};

class BmcWebSocketConnection {
public:
    ~BmcWebSocketConnection();

    BmcWebSocketConnection(BmcWebSocketConnection&&) = delete;
    BmcWebSocketConnection& operator=(BmcWebSocketConnection&&) = delete;
    BmcWebSocketConnection(const BmcWebSocketConnection&) = delete;
    BmcWebSocketConnection& operator=(const BmcWebSocketConnection&) = delete;

    boost::asio::io_context& io_context();
    std::shared_ptr<BmcWebSocketStream> stream();
    const std::string& role() const;
    void force_close() noexcept;

private:
    friend class BmcWebSession;

    explicit BmcWebSocketConnection(std::string role);

    boost::asio::io_context io_;
    boost::asio::ssl::context tls_context_;
    std::shared_ptr<BmcWebSocketStream> stream_;
    std::string role_;
};

using BmcWebSocketConnectionPtr = std::shared_ptr<BmcWebSocketConnection>;

struct BmcWebSocketOpenResult {
    BmcWebSocketConnectionPtr connection;
    boost::beast::websocket::response_type response;
};

class BmcWebSession {
public:
    explicit BmcWebSession(const LoginOptions& options);
    ~BmcWebSession();

    BmcWebSession(BmcWebSession&&) noexcept = default;
    BmcWebSession& operator=(BmcWebSession&&) noexcept = default;
    BmcWebSession(const BmcWebSession&) = delete;
    BmcWebSession& operator=(const BmcWebSession&) = delete;

    StringResponse request(
        http::verb method,
        std::string_view target,
        std::string body,
        std::string_view content_type,
        const std::vector<Header>& extra_headers = {});

    BmcWebSocketOpenResult open_websocket(BmcWebSocketConnectOptions options);
    void force_close_websocket(std::string_view role) noexcept;
    void close_all_websockets() noexcept;

    std::size_t cookie_count() const;
    std::string_view session_token() const;
    void set_cookie(std::string name, std::string value);

private:
    friend BmcWebSession login_bmc_web_session(const LoginOptions& options, const BmcLoginProfile& profile);

    void set_session_token(std::string token, std::string_view token_cookie_name);

    Url base_url_;
    bool insecure_ = false;
    bool verbose_ = false;
    CookieJar cookies_;
    std::string session_token_;
    std::unique_ptr<TlsSessionCache> tls_session_cache_;
    HttpsClient client_;
    struct WebSocketRegistry;
    std::unique_ptr<WebSocketRegistry> websockets_;
};

BmcWebSession login_bmc_web_session(const LoginOptions& options, const BmcLoginProfile& profile);

} // namespace hitsc
