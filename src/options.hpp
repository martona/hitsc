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
    bool debug_disable_http_keepalive = false;
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

enum class PikvmVideoDecodeMode {
    auto_select,
    software,
    d3d11,
};

struct PikvmViewOptions {
    LoginOptions login;
    int idle_timeout_seconds = 0;
    PikvmVideoDecodeMode video_decode = PikvmVideoDecodeMode::auto_select;
};

} // namespace hitsc
