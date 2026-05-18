#pragma once

#include "cookie_jar.hpp"
#include "options.hpp"

namespace hitsc {

struct PikvmSession {
    CookieJar cookies;
};

PikvmSession login_pikvm(const LoginOptions& options);
bool logout_pikvm(const LoginOptions& options, CookieJar& cookies);

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
