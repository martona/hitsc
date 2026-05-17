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

struct KvmProbeOptions {
    LoginOptions login;
    std::string capture_path;
    int packet_limit = 80;
    int idle_timeout_seconds = 10;
};

struct KvmViewOptions {
    LoginOptions login;
    int idle_timeout_seconds = 0;
};

} // namespace hitsc
