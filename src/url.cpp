#include "url.hpp"

#include "errors.hpp"

namespace hitsc {
namespace {

bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string strip_brackets(std::string_view host)
{
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        return std::string(host.substr(1, host.size() - 2));
    }

    return std::string(host);
}

} // namespace

Url parse_https_url(std::string_view raw_url)
{
    constexpr std::string_view scheme = "https://";
    if (!starts_with(raw_url, scheme)) {
        throw UserError("expected an https:// URL");
    }

    std::string_view rest = raw_url.substr(scheme.size());
    const auto fragment_start = rest.find('#');
    if (fragment_start != std::string_view::npos) {
        rest = rest.substr(0, fragment_start);
    }

    const auto target_start = rest.find_first_of("/?");
    std::string_view authority = rest.substr(0, target_start);
    std::string_view target_part =
        target_start == std::string_view::npos ? std::string_view{} : rest.substr(target_start);

    if (authority.empty()) {
        throw UserError("URL host is empty");
    }

    if (authority.find('@') != std::string_view::npos) {
        throw UserError("URL userinfo is not supported");
    }

    std::string_view host = authority;
    std::string_view port = "443";

    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            throw UserError("IPv6 address is missing ']'");
        }

        host = authority.substr(0, close + 1);
        const auto suffix = authority.substr(close + 1);
        if (!suffix.empty()) {
            if (suffix.front() != ':' || suffix.size() == 1) {
                throw UserError("invalid port after IPv6 address");
            }
            port = suffix.substr(1);
        }
    } else {
        const auto first_colon = authority.find(':');
        const auto last_colon = authority.rfind(':');
        if (first_colon != last_colon) {
            throw UserError("IPv6 addresses must be wrapped in []");
        }

        if (last_colon != std::string_view::npos) {
            if (last_colon == authority.size() - 1) {
                throw UserError("URL port is empty");
            }
            host = authority.substr(0, last_colon);
            port = authority.substr(last_colon + 1);
        }
    }

    if (host.empty()) {
        throw UserError("URL host is empty");
    }

    std::string target = target_part.empty() ? "/" : std::string(target_part);
    if (target.front() == '?') {
        target.insert(target.begin(), '/');
    }

    return Url{strip_brackets(host), std::string(port), std::move(target)};
}

std::string make_host_header(const Url& url)
{
    std::string host = url.host.find(':') == std::string::npos ? url.host : "[" + url.host + "]";
    if (url.port != "443") {
        host += ":" + url.port;
    }

    return host;
}

std::string make_origin(const Url& url)
{
    return "https://" + make_host_header(url);
}

} // namespace hitsc
