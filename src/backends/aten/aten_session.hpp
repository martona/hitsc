#pragma once

#include "bmc_session.hpp"
#include "options.hpp"

#include <string>

namespace hitsc {

// How the BMC authenticated us. Older firmware exposes the classic form login
// (/cgi/login.cgi); newer firmware (observed on Supermicro BMC 01.09.05) rejects
// it with HTTP 400 and instead opens a Redfish session whose response also sets
// the SID cookie that the CGI pages and the KVM websocket authenticate with.
enum class AtenLoginDialect {
    Legacy,
    Redfish,
};

struct AtenSession {
    BmcWebSession web;
    AtenLoginDialect dialect = AtenLoginDialect::Legacy;
    std::string redfish_session_id;  // Redfish dialect only
    std::string redfish_auth_token;  // Redfish dialect only
};

AtenSession login_aten(const LoginOptions& options);
bool logout_aten(const LoginOptions& options, AtenSession& session);

// One Redfish session POST (the newer firmware's login). Does not throw on an
// HTTP-level rejection -- login_aten uses it as the fallback dialect, and the
// cold reset uses it to obtain an X-Auth-Token on legacy-dialect firmware.
struct AtenRedfishLogin {
    int status = 0;           // HTTP status of the Sessions POST
    std::string session_id;
    std::string auth_token;
    std::string error_body;   // body snippet when status is not 2xx
};

AtenRedfishLogin login_aten_redfish(BmcWebSession& web, const LoginOptions& options);
std::string fetch_aten_ikvm_bootstrap(const LoginOptions& options, BmcWebSession& web);

class AtenLogoutGuard {
public:
    explicit AtenLogoutGuard(const LoginOptions& options);
    ~AtenLogoutGuard();

    AtenLogoutGuard(const AtenLogoutGuard&) = delete;
    AtenLogoutGuard& operator=(const AtenLogoutGuard&) = delete;

    void arm(AtenSession& session);
    void dismiss();

private:
    const LoginOptions& options_;
    AtenSession* session_ = nullptr;
    bool active_ = true;
};

} // namespace hitsc
