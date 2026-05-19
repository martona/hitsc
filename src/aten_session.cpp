#include "aten_session.hpp"

#include "bmc_session.hpp"
#include "http_client.hpp"
#include "log.hpp"
#include "text.hpp"
#include "url.hpp"

#include <boost/beast/http.hpp>

#include <chrono>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hitsc {
namespace http = boost::beast::http;

namespace {

bool is_redirect_status(int status)
{
    return status >= 300 && status < 400;
}

std::string redirect_target(const LoginOptions& options, const StringResponse& response)
{
    const auto location = response.find(http::field::location);
    if (location == response.end()) {
        return {};
    }

    std::string value = trim_copy(location->value());
    if (value.empty()) {
        return {};
    }

    if (value.starts_with("https://")) {
        Url redirected = parse_https_url(value);
        if (redirected.host != options.base_url.host || redirected.port != options.base_url.port) {
            throw std::runtime_error("aten iKVM bootstrap redirected to a different host: " + value);
        }
        return redirected.target;
    }

    if (value.starts_with("http://")) {
        throw std::runtime_error("aten iKVM bootstrap redirected to non-HTTPS URL: " + value);
    }

    if (value.front() == '/') {
        return value;
    }

    return "/" + value;
}

bool is_attribute_boundary(char ch)
{
    const unsigned char uch = static_cast<unsigned char>(ch);
    return !std::isalnum(uch) && ch != '_' && ch != '-';
}

std::string html_unescape_basic(std::string_view value)
{
    std::string decoded;
    decoded.reserve(value.size());

    for (std::size_t i = 0; i < value.size();) {
        if (value.compare(i, 5, "&amp;") == 0) {
            decoded.push_back('&');
            i += 5;
        } else if (value.compare(i, 6, "&quot;") == 0) {
            decoded.push_back('"');
            i += 6;
        } else if (value.compare(i, 5, "&#39;") == 0) {
            decoded.push_back('\'');
            i += 5;
        } else if (value.compare(i, 4, "&lt;") == 0) {
            decoded.push_back('<');
            i += 4;
        } else if (value.compare(i, 4, "&gt;") == 0) {
            decoded.push_back('>');
            i += 4;
        } else {
            decoded.push_back(value[i]);
            ++i;
        }
    }

    return decoded;
}

std::string html_attribute_value(std::string_view tag, std::string_view attribute)
{
    const std::string lowered_tag = lower_copy(tag);
    const std::string lowered_attribute = lower_copy(attribute);

    std::size_t search = 0;
    while ((search = lowered_tag.find(lowered_attribute, search)) != std::string::npos) {
        const std::size_t after_name = search + lowered_attribute.size();
        const bool before_ok = search == 0 || is_attribute_boundary(lowered_tag[search - 1]);
        const bool after_ok = after_name >= lowered_tag.size()
            || lowered_tag[after_name] == '='
            || std::isspace(static_cast<unsigned char>(lowered_tag[after_name]));
        if (!before_ok || !after_ok) {
            search = after_name;
            continue;
        }

        std::size_t pos = after_name;
        while (pos < lowered_tag.size()
               && std::isspace(static_cast<unsigned char>(lowered_tag[pos]))) {
            ++pos;
        }
        if (pos >= lowered_tag.size() || lowered_tag[pos] != '=') {
            search = after_name;
            continue;
        }
        ++pos;
        while (pos < lowered_tag.size()
               && std::isspace(static_cast<unsigned char>(lowered_tag[pos]))) {
            ++pos;
        }
        if (pos >= tag.size()) {
            return {};
        }

        std::size_t value_begin = pos;
        std::size_t value_end = pos;
        if (tag[pos] == '"' || tag[pos] == '\'') {
            const char quote = tag[pos];
            value_begin = pos + 1;
            value_end = tag.find(quote, value_begin);
            if (value_end == std::string_view::npos) {
                return {};
            }
        } else {
            while (value_end < tag.size()
                   && !std::isspace(static_cast<unsigned char>(tag[value_end]))
                   && tag[value_end] != '>') {
                ++value_end;
            }
        }

        return html_unescape_basic(tag.substr(value_begin, value_end - value_begin));
    }

    return {};
}

std::string extract_entry_value_from_html(std::string_view body)
{
    const std::string lowered_body = lower_copy(body);
    std::size_t search = 0;
    while ((search = lowered_body.find("entry_value", search)) != std::string::npos) {
        const std::size_t tag_begin = body.rfind('<', search);
        const std::size_t tag_end = body.find('>', search);
        if (tag_begin != std::string_view::npos
            && tag_end != std::string_view::npos
            && tag_begin < search
            && search < tag_end) {
            const std::string_view tag = body.substr(tag_begin, tag_end - tag_begin + 1);
            const std::string id = html_attribute_value(tag, "id");
            const std::string name = html_attribute_value(tag, "name");
            if (lower_copy(id) == "entry_value" || lower_copy(name) == "entry_value") {
                const std::string value = html_attribute_value(tag, "value");
                if (!value.empty()) {
                    return value;
                }
            }
        }

        search += std::string_view("entry_value").size();
    }

    return {};
}

const BmcLoginProfile& aten_login_profile()
{
    static const BmcLoginProfile profile{
        "aten",
        "/cgi/login.cgi",
        {
            BmcLoginField{"?name", BmcCredentialSource::Username, {}, BmcCredentialTransform::Base64, false},
            BmcLoginField{"pwd", BmcCredentialSource::Password, {}, BmcCredentialTransform::Base64},
            BmcLoginField{"check", BmcCredentialSource::Literal, "00", BmcCredentialTransform::None},
        },
        "application/x-www-form-urlencoded",
        nullptr,
        {},
        399,
    };
    return profile;
}

} // namespace

AtenSession login_aten(const LoginOptions& options)
{
    return AtenSession{login_bmc_web_session(options, aten_login_profile())};
}

bool logout_aten(const LoginOptions& options, BmcWebSession& web)
{
    const auto started_at = std::chrono::steady_clock::now();
    const auto log_duration = [&] {
        if (!options.verbose) {
            return;
        }
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at).count();
        log_info() << "aten logout duration-ms=" << elapsed_ms;
    };

    try {
        auto response = web.request(
            http::verb::get,
            "/cgi/logout.cgi",
            {},
            {});

        if (response.result_int() >= 200 && response.result_int() < 300) {
            if (options.verbose) {
                log_info() << "aten logout succeeded";
            }
            log_duration();
            return true;
        }

        log_warning() << "aten logout warning: HTTP "
                      << response.result_int() << ": "
                      << body_snippet(decode_response_body(response));
    } catch (const std::exception& ex) {
        log_warning() << "aten logout warning: " << ex.what();
    }

    log_duration();
    return false;
}

AtenLogoutGuard::AtenLogoutGuard(const LoginOptions& options)
    : options_(options)
{
}

AtenLogoutGuard::~AtenLogoutGuard()
{
    if (active_ && session_ != nullptr) {
        logout_aten(options_, session_->web);
    }
}

void AtenLogoutGuard::arm(AtenSession& session)
{
    session_ = &session;
}

void AtenLogoutGuard::dismiss()
{
    active_ = false;
}

std::string fetch_aten_ikvm_bootstrap(const LoginOptions& options, BmcWebSession& web)
{
    std::string target = "/cgi/url_redirect.cgi?url_name=man_ikvm_html5_bootstrap";
    int last_status = 0;
    for (int redirects = 0; redirects < 4; ++redirects) {
        auto response = web.request(
            http::verb::get,
            target,
            {},
            {});

        last_status = response.result_int();
        if (last_status >= 200 && last_status < 300) {
            const std::string body = decode_response_body(response);
            const std::string entry_value = extract_entry_value_from_html(body);
            if (options.verbose) {
                log_info() << "aten iKVM bootstrap touched"
                           << " target=" << target
                           << " http=" << last_status
                           << " entry-value=" << (entry_value.empty() ? "missing" : "present")
                           << " entry-value-bytes=" << entry_value.size();
            }
            return entry_value;
        }

        if (is_redirect_status(last_status)) {
            const std::string next_target = redirect_target(options, response);
            if (!next_target.empty()) {
                target = next_target;
                continue;
            }
        }

        throw std::runtime_error(
            "aten iKVM bootstrap failed with HTTP "
            + std::to_string(last_status) + ": "
            + body_snippet(decode_response_body(response)));
    }

    throw std::runtime_error(
        "aten iKVM bootstrap followed too many redirects; last HTTP "
        + std::to_string(last_status));
}

} // namespace hitsc
