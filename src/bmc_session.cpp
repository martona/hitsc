#include "bmc_session.hpp"

#include "errors.hpp"
#include "text.hpp"
#include "url.hpp"

#include <boost/beast/http.hpp>

#include <cstdint>
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
        body += field.encode_name ? form_url_encode(field.name) : field.name;
        body += '=';
        body += form_url_encode(value);
    }
    return body;
}

} // namespace

BmcWebSession::BmcWebSession(const LoginOptions& options)
    : base_url_(options.base_url)
    , tls_session_cache_(std::make_unique<TlsSessionCache>(16))
    , client_(
          options.base_url,
          options.insecure,
          options.verbose,
          30,
          tls_session_cache_.get(),
          !options.debug_disable_http_keepalive)
{
}

StringResponse BmcWebSession::request(
    http::verb method,
    std::string_view target,
    std::string body,
    std::string_view content_type,
    const std::vector<Header>& extra_headers)
{
    std::vector<Header> headers;
    headers.reserve(2 + extra_headers.size());

    const std::string origin = make_origin(base_url_);
    headers.push_back(Header{http::field::origin, {}, origin});
    headers.push_back(Header{http::field::referer, {}, origin + "/"});
    headers.insert(headers.end(), extra_headers.begin(), extra_headers.end());

    return client_.request(
        method,
        target,
        std::move(body),
        content_type,
        &cookies_,
        headers);
}

CookieJar& BmcWebSession::cookies()
{
    return cookies_;
}

const CookieJar& BmcWebSession::cookies() const
{
    return cookies_;
}

std::string_view BmcWebSession::session_token() const
{
    return session_token_;
}

void BmcWebSession::set_cookie(std::string name, std::string value)
{
    cookies_.set(std::move(name), std::move(value));
}

void BmcWebSession::set_session_token(std::string token, std::string_view token_cookie_name)
{
    session_token_ = std::move(token);
    if (!session_token_.empty() && !token_cookie_name.empty()) {
        cookies_.set(std::string(token_cookie_name), session_token_);
    }
}

BmcWebSession login_bmc_web_session(const LoginOptions& options, const BmcLoginProfile& profile)
{
    BmcWebSession session(options);
    auto response = session.request(
        http::verb::post,
        profile.login_target,
        build_form_body(options, profile),
        profile.content_type);

    const int status = static_cast<int>(response.result_int());
    if (status < 200 || status > profile.max_success_status) {
        const std::string body = decode_response_body(response);
        throw UserError(
            profile.vendor_name + " login failed with HTTP "
            + std::to_string(status) + ": " + body_snippet(body));
    }

    const std::string decoded_body = decode_response_body(response);
    if (profile.token_parser != nullptr) {
        session.set_session_token(profile.token_parser(decoded_body), profile.token_cookie_name);
    }

    return session;
}

} // namespace hitsc
