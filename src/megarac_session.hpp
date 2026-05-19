#pragma once

#include "bmc_session.hpp"
#include "options.hpp"

#include <string_view>

namespace hitsc {

struct MegaRacSession {
    BmcWebSession web;

    std::string_view csrf_token() const
    {
        return web.session_token();
    }
};

MegaRacSession login_megarac(const LoginOptions& options);
bool logout_megarac(const LoginOptions& options, BmcWebSession& web);

class MegaRacLogoutGuard {
public:
    explicit MegaRacLogoutGuard(const LoginOptions& options);
    ~MegaRacLogoutGuard();

    MegaRacLogoutGuard(const MegaRacLogoutGuard&) = delete;
    MegaRacLogoutGuard& operator=(const MegaRacLogoutGuard&) = delete;

    void arm(MegaRacSession& session);
    void dismiss();

private:
    const LoginOptions& options_;
    MegaRacSession* session_ = nullptr;
    bool active_ = true;
};

} // namespace hitsc
