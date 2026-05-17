#pragma once

#include "cookie_jar.hpp"
#include "options.hpp"

namespace hitsc {

struct AtenSession {
    CookieJar cookies;
};

AtenSession login_aten(const LoginOptions& options);
void fetch_aten_ikvm_bootstrap(const LoginOptions& options, CookieJar& cookies);

} // namespace hitsc
