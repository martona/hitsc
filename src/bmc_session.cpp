#include "bmc_session.hpp"

#include "http_client.hpp"
#include "text.hpp"
#include "url.hpp"

#include <boost/beast/http.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hitsc {
namespace http = boost::beast::http;

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
        body += form_url_encode(field.name);
        body += '=';
        body += form_url_encode(value);
    }
    return body;
}

} // namespace

BmcWebSession login_bmc_web_session(const LoginOptions& options, const BmcLoginProfile& profile)
{
    CookieJar cookies;
    const std::vector<Header> headers{
        Header{http::field::origin, {}, make_origin(options.base_url)},
        Header{http::field::referer, {}, make_origin(options.base_url) + "/"},
    };

    auto response = https_request(
        options.base_url,
        options.insecure,
        http::verb::post,
        profile.login_target,
        build_form_body(options, profile),
        profile.content_type,
        &cookies,
        headers,
        options.verbose);

    const int status = static_cast<int>(response.result_int());
    if (status < 200 || status > profile.max_success_status) {
        const std::string body = decode_response_body(response);
        throw std::runtime_error(
            profile.vendor_name + " login failed with HTTP "
            + std::to_string(status) + ": " + body_snippet(body));
    }

    const std::string decoded_body = decode_response_body(response);
    BmcWebSession session;
    session.cookies = std::move(cookies);
    if (profile.token_parser != nullptr) {
        session.session_token = profile.token_parser(decoded_body);
        if (!session.session_token.empty() && !profile.token_cookie_name.empty()) {
            session.cookies.set(profile.token_cookie_name, session.session_token);
        }
    }

    return session;
}

} // namespace hitsc
