#pragma once

#include "cookie_jar.hpp"
#include "url.hpp"

#include <boost/beast/http.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hitsc {

namespace http = boost::beast::http;

class TlsSessionCache;

struct Header {
    http::field field = http::field::unknown;
    std::string name;
    std::string value;
};

using StringResponse = http::response<http::string_body>;

class HttpsClient {
public:
    HttpsClient(
        const Url& url,
        bool insecure,
        bool verbose = false,
        int timeout_seconds = 30,
        TlsSessionCache* tls_session_cache = nullptr,
        bool keep_alive = true);
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string decode_response_body(const StringResponse& response);
void require_success_status(const StringResponse& response, std::string_view context);

} // namespace hitsc
