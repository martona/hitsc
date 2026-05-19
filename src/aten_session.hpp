#pragma once

#include "bmc_session.hpp"
#include "options.hpp"

#include <string>

namespace hitsc {

struct AtenSession {
    BmcWebSession web;
};

AtenSession login_aten(const LoginOptions& options);
bool logout_aten(const LoginOptions& options, BmcWebSession& web);
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
