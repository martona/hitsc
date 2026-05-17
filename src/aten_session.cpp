#include "aten_session.hpp"

#include "bmc_session.hpp"
#include "http_client.hpp"
#include "text.hpp"
#include "url.hpp"

#include <boost/beast/http.hpp>

#include <iostream>
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

const BmcLoginProfile& aten_login_profile()
{
    static const BmcLoginProfile profile{
        "aten",
        "/cgi/login.cgi",
        {
            BmcLoginField{"?name", BmcCredentialSource::Username, {}, BmcCredentialTransform::Base64},
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
    BmcWebSession web_session = login_bmc_web_session(options, aten_login_profile());
    AtenSession session;
    session.cookies = std::move(web_session.cookies);
    return session;
}

void fetch_aten_ikvm_bootstrap(const LoginOptions& options, CookieJar& cookies)
{
    const std::vector<Header> headers{
        Header{http::field::origin, {}, make_origin(options.base_url)},
        Header{http::field::referer, {}, make_origin(options.base_url) + "/"},
    };

    std::string target = "/cgi/url_redirect.cgi?url_name=man_ikvm_html5_bootstrap";
    int last_status = 0;
    for (int redirects = 0; redirects < 4; ++redirects) {
        auto response = https_request(
            options.base_url,
            options.insecure,
            http::verb::get,
            target,
            {},
            {},
            &cookies,
            headers,
            options.verbose);

        last_status = response.result_int();
        if (last_status >= 200 && last_status < 300) {
            std::cout << "hitsc: aten iKVM bootstrap touched"
                      << " target=" << target
                      << " http=" << last_status << '\n';
            return;
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
