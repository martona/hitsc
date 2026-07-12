#include "bmc_cold_reset.hpp"

#include "backends/auto/auto_view.hpp"
#include "backends/megarac/megarac_session.hpp"
#include "console_screen.hpp"
#include "errors.hpp"
#include "gui/viewer/qt_viewer_host.hpp"
#include "gui/viewer/viewer_window.hpp"
#include "http_client.hpp"
#include "log.hpp"
#include "tls_session_cache.hpp"

#include <boost/beast/http.hpp>

#include <QObject>

#include <atomic>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hitsc {
namespace {

namespace http = boost::beast::http;

// SP-X REST reboot: POST /api/maintenance/reset. Present even on firmware whose
// web UI has no restart button (older AST2500-era builds). Deliberately no
// logout -- the BMC reboots on acceptance, so the session dies with it.
void request_megarac_cold_reset(const LoginOptions& login)
{
    MegaRacSession session = login_megarac(login);
    log_info() << "megarac login succeeded";

    std::vector<Header> headers;
    const std::string_view csrf_token = session.web.session_token();
    if (!csrf_token.empty()) {
        headers.push_back(Header{http::field::unknown, "X-CSRFTOKEN", std::string(csrf_token)});
    }

    auto response = session.web.request(
        http::verb::post,
        "/api/maintenance/reset",
        {},
        {},
        headers);
    require_success_status(response, "/api/maintenance/reset");
    log_info() << "BMC accepted the cold reset request";
}

void perform_cold_reset(BmcColdResetOptions& options)
{
    if (options.detect_backend) {
        const std::string backend = detect_kvm_backend_name(options.login);
        if (backend != "megarac") {
            // TODO: investigate cold-reset paths for the other backends -- ATEN
            // likely has its own maintenance endpoint, and Redfish
            // Manager.Reset exists on some firmware; PiKVM restarts via its API.
            throw UserError(
                "unsupported BMC type for cold reset: "
                + (backend.empty() ? std::string("unknown") : backend)
                + " (only MegaRAC is supported)");
        }
    }

    request_megarac_cold_reset(options.login);
}

std::string error_message(const std::exception_ptr& error)
{
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& ex) {
        return ex.what();
    } catch (...) {
        return "unknown error";
    }
}

// Mirrors AutoDetection in auto_view.cpp: the worker runs the HTTP work while
// the harness shows the connecting console; the window's frameTick polls for
// completion. The destructor cancels + joins so the raw pointer never dangles.
struct ColdResetWork {
    BmcColdResetOptions options;
    std::atomic_bool done{false};
    std::exception_ptr error;
    std::thread worker;
    bool handled = false;

    ~ColdResetWork()
    {
        if (options.login.cancel_token) {
            options.login.cancel_token->cancel();
        }
        if (worker.joinable()) {
            worker.join();
        }
    }
};

} // namespace

int run_bmc_cold_reset(const BmcColdResetOptions& options)
{
    auto failed = std::make_shared<bool>(false);

    // Same window shell as a KVM session (title, geometry persistence, cert
    // prompts), labeled so it doesn't read as a stuck connection attempt.
    const ViewerLaunch launch{
        options.login.base_url.host + " (BMC cold reset)",
        options.login.host_id,
    };
    run_viewer(launch, [options, failed](ViewerHost& host) {
        auto work = std::make_shared<ColdResetWork>();
        work->options = options;
        if (!work->options.login.tls_session_cache) {
            work->options.login.tls_session_cache = std::make_shared<TlsSessionCache>(16);
        }
        // Makes the HTTP work abortable from ~ColdResetWork (window closed mid-flight).
        if (!work->options.login.cancel_token) {
            work->options.login.cancel_token = std::make_shared<HttpCancelToken>();
        }

        ColdResetWork* state = work.get();
        work->worker = std::thread([state]() {
            try {
                perform_cold_reset(state->options);
            } catch (...) {
                state->error = std::current_exception();
            }
            state->done.store(true);
        });

        // Unlike auto-detection, the outcome is displayed IN the window (success
        // and failure both), not routed to fail() -- the whole point of running
        // this in the viewer shell is that the user gets to read the output.
        QObject::connect(
            &host.window(), &ViewerWindow::frameTick, &host.window(), [work, failed, &host]() {
                if (work->handled || !work->done.load()) {
                    return;
                }
                work->handled = true;
                if (work->worker.joinable()) {
                    work->worker.join();
                }

                ConsoleScreen screen;
                if (work->error) {
                    *failed = true;
                    screen.severity = ConsoleSeverity::Error;
                    screen.headline = "BMC cold reset failed";
                    screen.detail = error_message(work->error);
                } else {
                    screen.headline = "BMC cold reset requested";
                    screen.detail =
                        "The BMC is rebooting and should be reachable again in a minute or two. "
                        "The host's power state is not affected.";
                }
                screen.hint = "Esc to close";
                host.set_console(screen);
            });
    });

    return *failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

} // namespace hitsc
