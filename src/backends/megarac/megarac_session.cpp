#include "megarac_session.hpp"

#include "bmc_session.hpp"
#include "http_client.hpp"
#include "log.hpp"
#include "text.hpp"
#include "url.hpp"

#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hitsc {
namespace http = boost::beast::http;
namespace json = boost::json;

namespace {

std::string csrf_token_from_body(std::string_view body)
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
    static const BmcLoginProfile profile{
        "megarac",
        "/api/session",
        {
            BmcLoginField{"username", BmcCredentialSource::Username},
            BmcLoginField{"password", BmcCredentialSource::Password},
        },
        "application/x-www-form-urlencoded",
        csrf_token_from_body,
        "__Host-garc",
        299,
    };

    return MegaRacSession{login_bmc_web_session(options, profile)};
}

bool logout_megarac(const LoginOptions& options, BmcWebSession& web)
{
    (void)options;
    web.close_all_websockets();

    std::vector<Header> headers;
    const std::string_view csrf_token = web.session_token();
    if (!csrf_token.empty()) {
        headers.push_back(Header{http::field::unknown, "X-CSRFTOKEN", std::string(csrf_token)});
    }

    try {
        auto response = web.request(
            http::verb::delete_,
            "/api/session",
            {},
            {},
            headers);

        if (response.result_int() >= 200 && response.result_int() < 300) {
            log_info() << "megarac logout succeeded";
            return true;
        }

        log_warning() << "megarac logout warning: HTTP "
                      << response.result_int() << ": "
                      << body_snippet(decode_response_body(response));
    } catch (const std::exception& ex) {
        log_warning() << "megarac logout warning: " << ex.what();
    }

    return false;
}

MegaRacLogoutGuard::MegaRacLogoutGuard(const LoginOptions& options)
    : options_(options)
{
}

MegaRacLogoutGuard::~MegaRacLogoutGuard()
{
    if (active_ && session_ != nullptr) {
        logout_megarac(options_, session_->web);
    }
}

void MegaRacLogoutGuard::arm(MegaRacSession& session)
{
    session_ = &session;
}

void MegaRacLogoutGuard::dismiss()
{
    active_ = false;
}

} // namespace hitsc
