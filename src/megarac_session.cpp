#include "megarac_session.hpp"

#include "http_client.hpp"
#include "text.hpp"
#include "url.hpp"

#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace hitsc {
namespace http = boost::beast::http;
namespace json = boost::json;

namespace {

std::string csrf_token_from_body(const std::string& body)
{
    boost::system::error_code error;
    json::value value = json::parse(body, error);
    if (error) {
        return {};
    }

    const auto* object = value.if_object();
    if (object == nullptr) {
        return {};
    }

    const json::value* token = object->if_contains("CSRFToken");
    if (token == nullptr) {
        return {};
    }

    const json::string* token_string = token->if_string();
    if (token_string == nullptr) {
        return {};
    }

    return std::string(*token_string);
}

} // namespace

MegaRacSession login_megarac(const LoginOptions& options)
{
    std::string body = "username=" + form_url_encode(options.username);
    body += "&password=" + form_url_encode(options.password);

    CookieJar cookies;
    const std::vector<Header> headers{
        Header{http::field::origin, {}, make_origin(options.base_url)},
        Header{http::field::referer, {}, make_origin(options.base_url) + "/"},
    };

    auto response = https_request(
        options.base_url,
        options.insecure,
        http::verb::post,
        "/api/session",
        std::move(body),
        "application/x-www-form-urlencoded",
        &cookies,
        headers,
        options.verbose);

    require_success_status(response, "/api/session");

    const std::string decoded_body = decode_response_body(response);
    MegaRacSession session;
    session.cookies = std::move(cookies);
    session.csrf_token = csrf_token_from_body(decoded_body);
    if (!session.csrf_token.empty()) {
        session.cookies.set("__Host-garc", session.csrf_token);
    }

    return session;
}

} // namespace hitsc
