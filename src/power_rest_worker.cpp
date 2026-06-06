#include "power_rest_worker.hpp"

#include "bmc_session.hpp"
#include "http_client.hpp"
#include "log.hpp"
#include "text.hpp"

#include <exception>
#include <string>
#include <utility>

namespace hitsc {

PowerRestWorker::PowerRestWorker(
    BmcWebSession& web,
    std::function<PowerRequestSpec(PowerAction)> build_request,
    std::function<void(PowerOutcome)> report)
    : web_(web)
    , build_request_(std::move(build_request))
    , report_(std::move(report))
{
    thread_ = std::thread([this] { run(); });
}

PowerRestWorker::~PowerRestWorker()
{
    stop();
}

void PowerRestWorker::submit(PowerAction action)
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        queue_.push_back(action);
    }
    cv_.notify_one();
}

void PowerRestWorker::stop() noexcept
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    // Abort any request currently blocked in run() so the join returns promptly.
    web_.cancel_in_flight_request();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void PowerRestWorker::run()
{
    while (true) {
        PowerAction action;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) {
                return;
            }
            action = queue_.front();
            queue_.pop_front();
        }

        PowerOutcome outcome;
        outcome.action = action;
        try {
            const PowerRequestSpec spec = build_request_(action);
            log_info() << "power: " << power_action_name(action);
            log_info() << "  -> POST " << spec.target;  // worker is REST POST-only (PiKVM)
            StringResponse response = web_.request(
                spec.method, spec.target, spec.body, spec.content_type, spec.headers);
            const int status = static_cast<int>(response.result_int());
            outcome.http_status = status;
            outcome.ok = status >= 200 && status < 300;
            if (outcome.ok) {
                log_info() << "  <- HTTP " << status;
            } else {
                outcome.detail = "HTTP ERROR " + std::to_string(status);
                log_warning() << "  <- HTTP " << status
                              << ": " << body_snippet(decode_response_body(response));
            }
        } catch (const std::exception& ex) {
            // stop() cancels the in-flight request, which surfaces here as a thrown
            // transport error -- swallow it (no toast) when we are tearing down.
            {
                std::lock_guard lock(mutex_);
                if (stopping_) {
                    return;
                }
            }
            outcome.ok = false;
            outcome.http_status = 0;
            outcome.detail = ex.what();
            log_warning() << "power " << power_action_name(action) << " failed: " << ex.what();
        }

        if (report_) {
            report_(std::move(outcome));
        }
    }
}

} // namespace hitsc
