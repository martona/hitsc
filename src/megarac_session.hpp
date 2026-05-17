#pragma once

#include "cookie_jar.hpp"
#include "options.hpp"

#include <string>
#include <string_view>

namespace hitsc {

struct MegaRacSession {
    CookieJar cookies;
    std::string csrf_token;
};

MegaRacSession login_megarac(const LoginOptions& options);
bool logout_megarac(const LoginOptions& options, CookieJar& cookies, std::string_view csrf_token);

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
