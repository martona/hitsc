#pragma once

#include "cookie_jar.hpp"
#include "url.hpp"

#include <boost/beast/http.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace boost::asio {
class io_context;
} // namespace boost::asio

namespace hitsc {

namespace http = boost::beast::http;

class TlsSessionCache;

struct Header {
    http::field field = http::field::unknown;
    std::string name;
    std::string value;
};

using StringResponse = http::response<http::string_body>;

// Cross-thread cancellation for HTTP work. One token may be shared by several
// HttpsClients (a session's login + logout, a detection probe). cancel() aborts any
// request() in flight on every bound client and makes their future requests fail
// immediately with operation_aborted. Sticky by design -- it exists for teardown,
// where "stop now and stay stopped" is the only correct answer.
class HttpCancelToken {
public:
    void cancel() noexcept;
    bool canceled() const noexcept;

private:
    friend class HttpsClient;  // Impl (nested, shares access) registers per-request io_contexts

    void register_io(boost::asio::io_context& io);
    void unregister_io(boost::asio::io_context& io) noexcept;

    mutable std::mutex mutex_;
    bool canceled_ = false;
    std::vector<boost::asio::io_context*> contexts_;
};

class HttpsClient {
public:
    HttpsClient(
        const Url& url,
        bool insecure,
        bool verbose = false,
        int timeout_seconds = 30,
        TlsSessionCache* tls_session_cache = nullptr,
        bool keep_alive = true,
        std::shared_ptr<HttpCancelToken> cancel_token = nullptr);
    ~HttpsClient();

    HttpsClient(HttpsClient&&) noexcept;
    HttpsClient& operator=(HttpsClient&&) noexcept;
    HttpsClient(const HttpsClient&) = delete;
    HttpsClient& operator=(const HttpsClient&) = delete;

    StringResponse request(
        http::verb method,
        std::string_view target,
        std::string body,
        std::string_view content_type,
        CookieJar* cookies,
        const std::vector<Header>& extra_headers = {});

    // Abort an in-flight request() from ANOTHER thread (io_context::stop is
    // thread-safe; every operation is async internally, pumped by request(), so the
    // stop genuinely unblocks it). Used to make a blocking power POST cancelable for
    // instant exit. Not sticky: the next request() reconnects normally.
    void cancel() noexcept;

    // Change the per-operation deadline for subsequent requests on this client.
    // Teardown paths (logout) shrink it so a dead BMC cannot hold exit hostage.
    // Call from the same thread that issues the requests.
    void set_timeout_seconds(int timeout_seconds) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string decode_response_body(const StringResponse& response);
void require_success_status(const StringResponse& response, std::string_view context);

} // namespace hitsc
