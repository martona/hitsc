#pragma once

#include <boost/beast/http.hpp>

#include <string_view>

namespace hitsc {

namespace http = boost::beast::http;

void log_http_request(const http::request<http::string_body>& request, bool enabled);
void log_http_response(const http::response<http::string_body>& response, std::string_view decoded_body, bool enabled);

} // namespace hitsc
