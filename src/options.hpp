#pragma once

#include "url.hpp"

#include <string>

namespace hitsc {

struct LoginOptions {
    Url base_url;
    std::string username;
    std::string password;
    bool verbose = false;
    bool insecure = false;
};

struct MegaracViewOptions {
    LoginOptions login;
    int idle_timeout_seconds = 0;
};

struct AtenViewOptions {
    LoginOptions login;
    int idle_timeout_seconds = 0;
    bool shared = true;
};

struct PikvmProbeOptions {
    LoginOptions login;
    int idle_timeout_seconds = 30;
};

} // namespace hitsc
