#pragma once

#include "http_client.hpp"    // Header, http::verb
#include "power_control.hpp"  // PowerAction, PowerOutcome

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hitsc {

class BmcWebSession;

// One REST request to issue for a power action.
struct PowerRequestSpec {
    http::verb method = http::verb::post;
    std::string target;
    std::string body;
    std::string content_type;
    std::vector<Header> headers;
};

// Runs blocking power REST calls (MegaRAC / PiKVM) on a dedicated thread so they never
// stall the video/input websocket I/O. Reuses the session's BmcWebSession (its cookies
// + CSRF), and is cancelable mid-flight: stop() aborts an in-flight request via
// BmcWebSession::cancel_in_flight_request() so app exit is immediate. Each result is
// reported as a PowerOutcome (for the GUI toast).
class PowerRestWorker {
public:
    PowerRestWorker(
        BmcWebSession& web,
        std::function<PowerRequestSpec(PowerAction)> build_request,
        std::function<void(PowerOutcome)> report);
    ~PowerRestWorker();

    PowerRestWorker(const PowerRestWorker&) = delete;
    PowerRestWorker& operator=(const PowerRestWorker&) = delete;

    void submit(PowerAction action);
    void stop() noexcept;  // idempotent: cancels in-flight + joins

private:
    void run();

    BmcWebSession& web_;
    std::function<PowerRequestSpec(PowerAction)> build_request_;
    std::function<void(PowerOutcome)> report_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<PowerAction> queue_;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace hitsc
