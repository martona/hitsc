#include "pikvm_session.hpp"

#include "bmc_session.hpp"
#include "errors.hpp"
#include "http_client.hpp"
#include "log.hpp"
#include "text.hpp"
#include "url.hpp"

#include <boost/beast/http.hpp>

#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace hitsc {
namespace http = boost::beast::http;

namespace {

const BmcLoginProfile& pikvm_login_profile()
{
    static const BmcLoginProfile profile{
        "pikvm",
        "/api/auth/login",
        {
            BmcLoginField{"user", BmcCredentialSource::Username},
            BmcLoginField{"passwd", BmcCredentialSource::Password},
        },
        "application/x-www-form-urlencoded",
        nullptr,
        {},
        299,
    };
    return profile;
}

} // namespace

PikvmSession login_pikvm(const LoginOptions& options)
{
    try {
        return PikvmSession{login_bmc_web_session(options, pikvm_login_profile())};
    } catch (const UserError&) {
        throw;
    } catch (const std::exception& ex) {
        throw UserError(std::string("pikvm login failed: ") + ex.what());
    }
}

bool logout_pikvm(const LoginOptions& options, BmcWebSession& web)
{
    try {
        auto response = web.request(
            http::verb::post,
            "/api/auth/logout",
            {},
            {});

        if (response.result_int() >= 200 && response.result_int() < 300) {
            if (options.verbose) {
                log_info() << "pikvm logout succeeded";
            }
            return true;
        }

        log_warning() << "pikvm logout warning: HTTP "
                      << response.result_int() << ": "
                      << body_snippet(decode_response_body(response));
    } catch (const std::exception& ex) {
        log_warning() << "pikvm logout warning: " << ex.what();
    }

    return false;
}

PikvmLogoutGuard::PikvmLogoutGuard(const LoginOptions& options)
    : options_(options)
{
}

PikvmLogoutGuard::~PikvmLogoutGuard()
{
    if (active_ && session_ != nullptr) {
        logout_pikvm(options_, session_->web);
    }
}

void PikvmLogoutGuard::arm(PikvmSession& session)
{
    session_ = &session;
}

void PikvmLogoutGuard::dismiss()
{
    active_ = false;
}

} // namespace hitsc
