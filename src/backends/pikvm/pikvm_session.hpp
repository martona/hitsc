#pragma once

#include "bmc_session.hpp"
#include "options.hpp"

namespace hitsc {

struct PikvmSession {
    BmcWebSession web;
};

PikvmSession login_pikvm(const LoginOptions& options);
bool logout_pikvm(const LoginOptions& options, BmcWebSession& web);

class PikvmLogoutGuard {
public:
    explicit PikvmLogoutGuard(const LoginOptions& options);
    ~PikvmLogoutGuard();

    PikvmLogoutGuard(const PikvmLogoutGuard&) = delete;
    PikvmLogoutGuard& operator=(const PikvmLogoutGuard&) = delete;

    void arm(PikvmSession& session);
    void dismiss();

private:
    const LoginOptions& options_;
    PikvmSession* session_ = nullptr;
    bool active_ = true;
};

} // namespace hitsc
