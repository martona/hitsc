#pragma once

#include "url.hpp"

#include <memory>
#include <string>

namespace hitsc {

class HttpCancelToken;
class TlsSessionCache;

struct VerbosityOptions {
    bool verbose = false;
    bool vverbose = false;
};

struct LoginOptions {
    Url base_url;
    std::string username;
    std::string password;
    bool verbose = false;
    bool vverbose = false;
    bool insecure = false;
    bool debug_disable_http_keepalive = false;
    std::shared_ptr<TlsSessionCache> tls_session_cache;
    // Cross-thread abort for this session's HTTP work (login, config fetches,
    // logout). Shared into every HttpsClient built from these options; sessions
    // arm their force-close slot with it during the login phase, when no socket
    // exists yet to close. Optional -- created on demand by the session runners.
    std::shared_ptr<HttpCancelToken> cancel_token;
    // Saved-host id, used to persist this viewer's window geometry with the host
    // record. Empty for direct-CLI launches (no saved host).
    std::string host_id;
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
};

struct PikvmViewOptions {
    LoginOptions login;
    int idle_timeout_seconds = 0;
    PikvmVideoDecodeMode video_decode = PikvmVideoDecodeMode::auto_select;
};

struct AutoViewOptions {
    LoginOptions login;
    int idle_timeout_seconds = 0;
    bool aten_shared = true;
    PikvmVideoDecodeMode pikvm_video_decode = PikvmVideoDecodeMode::auto_select;
};

} // namespace hitsc
