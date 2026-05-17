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

struct MegaracProbeOptions {
    LoginOptions login;
    std::string capture_path;
    int packet_limit = 80;
    int idle_timeout_seconds = 10;
};

struct MegaracViewOptions {
    LoginOptions login;
    int idle_timeout_seconds = 0;
};

struct AtenProbeOptions {
    LoginOptions login;
    bool fetch_bootstrap = true;
    std::string websocket_path = "/";
    int idle_timeout_seconds = 10;
};

} // namespace hitsc
