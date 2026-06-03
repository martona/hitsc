#include "auto_view.hpp"

#include "backends/aten/aten_view.hpp"
#include "backends/megarac/megarac_view.hpp"
#include "backends/pikvm/pikvm_view.hpp"
#include "cookie_jar.hpp"
#include "errors.hpp"
#include "http_client.hpp"
#include "log.hpp"
#include "text.hpp"
#include "tls_session_cache.hpp"
#include "view_base.hpp"
#include "view_console.hpp"

#include <boost/beast/http.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace hitsc {
namespace {

enum class DetectedKvmBackend {
    Unknown,
    Megarac,
    Aten,
    Pikvm,
};

struct FingerprintCandidate {
    DetectedKvmBackend backend = DetectedKvmBackend::Unknown;
    int score = 0;
    std::vector<std::string> reasons;
};

struct KvmBackendFingerprint {
    DetectedKvmBackend backend = DetectedKvmBackend::Unknown;
    int score = 0;
    std::vector<std::string> reasons;
};

std::string backend_name(DetectedKvmBackend backend)
{
    switch (backend) {
    case DetectedKvmBackend::Megarac:
        return "megarac";
    case DetectedKvmBackend::Aten:
        return "aten";
    case DetectedKvmBackend::Pikvm:
        return "pikvm";
    case DetectedKvmBackend::Unknown:
        break;
    }
    return "unknown";
}

std::string join_reasons(const std::vector<std::string>& reasons)
{
    std::string result;
    for (const std::string& reason : reasons) {
        if (!result.empty()) {
            result += ", ";
        }
        result += reason;
    }
    return result;
}

std::string header_value(const StringResponse& response, std::string_view name)
{
    const std::string normalized_name = lower_copy(std::string(name));
    for (const auto& field : response) {
        if (lower_copy(std::string(field.name_string())) == normalized_name) {
            return std::string(field.value());
        }
    }
    return {};
}

bool header_contains(const StringResponse& response, std::string_view name, std::string_view needle)
{
    const std::string value = lower_copy(header_value(response, name));
    return value.find(lower_copy(std::string(needle))) != std::string::npos;
}

bool header_equals(const StringResponse& response, std::string_view name, std::string_view expected)
{
    const std::string value = lower_copy(trim_copy(header_value(response, name)));
    return value == lower_copy(std::string(expected));
}

bool is_redirect(const StringResponse& response)
{
    const unsigned int status = response.result_int();
    return status >= 300 && status < 400;
}

void score_if(
    FingerprintCandidate& candidate,
    bool condition,
    int points,
    std::string reason)
{
    if (!condition) {
        return;
    }
    candidate.score += points;
    candidate.reasons.push_back(std::move(reason));
}

KvmBackendFingerprint classify_root_response(const StringResponse& response)
{
    FingerprintCandidate megarac{DetectedKvmBackend::Megarac};
    FingerprintCandidate aten{DetectedKvmBackend::Aten};
    FingerprintCandidate pikvm{DetectedKvmBackend::Pikvm};

    const std::string csp = lower_copy(header_value(response, "content-security-policy"));

    score_if(pikvm, is_redirect(response), 2, "HTTP redirect from /");
    score_if(pikvm, header_equals(response, "location", "/login"), 4, "Location: /login");
    score_if(pikvm, header_contains(response, "server", "nginx"), 2, "Server: nginx");

    score_if(megarac, response.result_int() == 200, 1, "HTTP 200 from /");
    score_if(megarac, header_contains(response, "server", "lighttpd"), 4, "Server: lighttpd");
    score_if(megarac, header_contains(response, "content-encoding", "gzip"), 1, "Content-Encoding: gzip");
    score_if(megarac, header_contains(response, "referrer-policy", "no-referrer"), 2, "Referrer-Policy: no-referrer");
    score_if(megarac, csp.find("object-src 'none'") != std::string::npos, 1, "CSP object-src 'none'");
    score_if(megarac, csp.find("frame-ancestors 'self'") != std::string::npos, 1, "CSP frame-ancestors 'self'");

    score_if(aten, response.result_int() == 200, 1, "HTTP 200 from /");
    score_if(aten, header_contains(response, "content-type", "charset=utf-8"), 1, "Content-Type charset=UTF-8");
    score_if(aten, header_equals(response, "cache-control", "private"), 1, "Cache-Control: private");
    score_if(aten, csp.find("'unsafe-eval'") != std::string::npos, 3, "CSP allows unsafe-eval");
    score_if(aten, csp.find("worker-src 'self' blob:") != std::string::npos, 2, "CSP worker-src blob");
    score_if(aten, csp.find("img-src 'self' data:") != std::string::npos, 1, "CSP img-src data");

    std::vector<FingerprintCandidate> candidates{megarac, aten, pikvm};
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return left.score > right.score;
    });

    const FingerprintCandidate& best = candidates[0];
    const FingerprintCandidate& second = candidates[1];
    if (best.score < 3 || best.score == second.score) {
        return KvmBackendFingerprint{};
    }
    return KvmBackendFingerprint{best.backend, best.score, best.reasons};
}

KvmBackendFingerprint detect_kvm_backend(LoginOptions& login)
{
    if (!login.tls_session_cache) {
        login.tls_session_cache = std::make_shared<TlsSessionCache>(16);
    }

    HttpsClient client(
        login.base_url,
        login.insecure,
        login.verbose,
        10,
        login.tls_session_cache.get(),
        false);
    CookieJar cookies;
    StringResponse response = client.request(http::verb::get, "/", {}, {}, &cookies);
    return classify_root_response(response);
}

std::string detection_failure_message()
{
    return "Could not auto-detect KVM type from GET /. Choose ATEN, MegaRAC, or PiKVM manually.";
}

std::string message_from_exception(std::exception_ptr exception)
{
    if (!exception) {
        return {};
    }
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "Unknown error";
    }
}

struct DetectionWork {
    AutoViewOptions options;
    std::atomic_bool done{false};
    DetectedKvmBackend backend = DetectedKvmBackend::Unknown;
    std::exception_ptr exception;
};

struct DetectionConsole {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    ConsoleScreen screen;

    void render() const
    {
        render_view_console(renderer, window, screen);
    }

    static bool SDLCALL on_event_watch(void* userdata, SDL_Event* event)
    {
        if (event != nullptr &&
            (event->type == SDL_EVENT_WINDOW_RESIZED ||
             event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
             event->type == SDL_EVENT_WINDOW_EXPOSED)) {
            static_cast<DetectionConsole*>(userdata)->render();
        }
        return true;
    }
};

// Shows the connecting/detecting console while detect_kvm_backend() runs on a
// worker thread. Returns the detected backend, or nullopt if the user closed
// the window. Detection failures show the error console with R-to-retry.
std::optional<DetectedKvmBackend> run_auto_detection(const ViewWindow& view_window, AutoViewOptions& options)
{
    if (!options.login.tls_session_cache) {
        options.login.tls_session_cache = std::make_shared<TlsSessionCache>(16);
    }

    DetectionConsole console;
    console.window = view_window.window;
    console.renderer = SDL_CreateRenderer(view_window.window, nullptr);
    if (console.renderer == nullptr) {
        throw std::runtime_error(std::string("SDL_CreateRenderer: ") + SDL_GetError());
    }
    SDL_AddEventWatch(DetectionConsole::on_event_watch, &console);

    const std::string host = options.login.base_url.host;

    std::shared_ptr<DetectionWork> work;
    std::thread worker;

    const auto start_worker = [&work, &worker, &options]() {
        work = std::make_shared<DetectionWork>();
        work->options = options; // shares the TLS session cache shared_ptr
        std::shared_ptr<DetectionWork> handle = work;
        worker = std::thread([handle]() {
            try {
                const KvmBackendFingerprint fingerprint = detect_kvm_backend(handle->options.login);
                if (fingerprint.backend == DetectedKvmBackend::Unknown) {
                    throw UserError(detection_failure_message());
                }
                log_info() << "auto KVM detection selected"
                           << " backend=" << backend_name(fingerprint.backend)
                           << " score=" << fingerprint.score
                           << " reason=" << join_reasons(fingerprint.reasons);
                handle->backend = fingerprint.backend;
            } catch (...) {
                handle->exception = std::current_exception();
            }
            handle->done.store(true);
        });
    };

    std::optional<DetectedKvmBackend> resolved;
    bool errored = false;
    std::string error_detail;
    std::uint64_t last_render = 0;
    bool running = true;

    start_worker();

    while (running) {
        if (errored) {
            console.screen.severity = ConsoleSeverity::Error;
            console.screen.headline = "Connection failed";
            console.screen.detail = error_detail;
            console.screen.hint = "Press R to reconnect      Esc to close";
        } else {
            console.screen.severity = ConsoleSeverity::Info;
            console.screen.headline = "Connecting to " + host + "...";
            console.screen.detail.clear();
            console.screen.hint = "Esc to cancel";
        }

        bool retry_requested = false;
        SDL_Event event{};
        bool have_event = SDL_WaitEventTimeout(&event, 16);
        while (have_event) {
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    running = false;
                } else if (errored && event.key.scancode == SDL_SCANCODE_R) {
                    retry_requested = true;
                }
            }
            have_event = SDL_PollEvent(&event);
        }

        if (!running) {
            break;
        }

        if (retry_requested) {
            if (worker.joinable()) {
                worker.join();
            }
            errored = false;
            error_detail.clear();
            last_render = 0;
            start_worker();
            continue;
        }

        const std::uint64_t ticks = SDL_GetTicks();
        if (last_render == 0 || ticks - last_render >= 150) {
            last_render = ticks;
            console.render();
        }

        if (!errored && work->done.load()) {
            if (worker.joinable()) {
                worker.join();
            }
            if (work->exception) {
                errored = true;
                error_detail = message_from_exception(work->exception);
                last_render = 0;
            } else {
                resolved = work->backend;
                running = false;
            }
        }
    }

    // If the user aborted while a probe is still in flight, detach it (the HTTP
    // request has its own timeout) instead of blocking the window on join.
    if (worker.joinable()) {
        if (work && !work->done.load()) {
            worker.detach();
        } else {
            worker.join();
        }
    }

    SDL_RemoveEventWatch(DetectionConsole::on_event_watch, &console);
    SDL_DestroyRenderer(console.renderer);

    return resolved;
}

} // namespace

void run_auto_view(const AutoViewOptions& options)
{
    // Create the window up front so detection runs on-screen (with the same
    // console + retry as a live session), then hand the window to the detected
    // backend, which takes over SDL ownership.
    ViewWindow view_window = make_view_window(options.login.host_id);

    const auto destroy_window = [&view_window, &options]() {
        if (view_window.window != nullptr) {
            // Remember the window position even if detection failed/was aborted.
            save_view_window_geometry(view_window.window, options.login.host_id);
            SDL_DestroyWindow(view_window.window);
            view_window.window = nullptr;
        }
        SDL_Quit();
    };

    AutoViewOptions detected_options = options;
    std::optional<DetectedKvmBackend> backend;
    try {
        backend = run_auto_detection(view_window, detected_options);
    } catch (...) {
        destroy_window();
        throw;
    }

    if (!backend) {
        destroy_window(); // user closed during detection
        return;
    }

    // From here the concrete view adopts the window and owns its teardown.
    switch (*backend) {
    case DetectedKvmBackend::Megarac: {
        MegaracViewOptions view_options;
        view_options.login = std::move(detected_options.login);
        view_options.idle_timeout_seconds = detected_options.idle_timeout_seconds;
        run_megarac_view(view_options, &view_window);
        return;
    }
    case DetectedKvmBackend::Aten: {
        AtenViewOptions view_options;
        view_options.login = std::move(detected_options.login);
        view_options.idle_timeout_seconds = detected_options.idle_timeout_seconds;
        view_options.shared = detected_options.aten_shared;
        run_aten_view(view_options, &view_window);
        return;
    }
    case DetectedKvmBackend::Pikvm: {
        PikvmViewOptions view_options;
        view_options.login = std::move(detected_options.login);
        view_options.idle_timeout_seconds = detected_options.idle_timeout_seconds;
        view_options.video_decode = detected_options.pikvm_video_decode;
        run_pikvm_view(view_options, &view_window);
        return;
    }
    case DetectedKvmBackend::Unknown:
        break;
    }

    destroy_window();
}

} // namespace hitsc
