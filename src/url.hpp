#pragma once

#include <string>
#include <string_view>

namespace hitsc {

struct Url {
    std::string host;
    std::string port;
    std::string target;
};

Url parse_https_url(std::string_view raw_url);
std::string make_host_header(const Url& url);
std::string make_origin(const Url& url);

} // namespace hitsc
