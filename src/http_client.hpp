#pragma once

#include "cookie_jar.hpp"
#include "url.hpp"

#include <boost/beast/http.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace hitsc {

namespace http = boost::beast::http;

struct Header {
    http::field field = http::field::unknown;
    std::string name;
    std::string value;
};

using StringResponse = http::response<http::string_body>;

StringResponse https_request(
    const Url& url,
    bool insecure,
    http::verb method,
    std::string_view target,
    std::string body,
    std::string_view content_type,
    CookieJar* cookies,
    const std::vector<Header>& extra_headers = {},
    bool verbose = false);

std::string decode_response_body(const StringResponse& response);
void require_success_status(const StringResponse& response, std::string_view context);

} // namespace hitsc
