#pragma once

#include "cookie_jar.hpp"
#include "options.hpp"

#include <string>

namespace hitsc {

struct MegaRacSession {
    CookieJar cookies;
    std::string csrf_token;
};

MegaRacSession login_megarac(const LoginOptions& options);

} // namespace hitsc
