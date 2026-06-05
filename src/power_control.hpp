#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace hitsc {

// A host power operation the user can request. Power-cycle is intentionally absent
// (these four are the full UI set). Each backend maps these onto its own transport:
// ATEN sends a websocket packet, MegaRAC/PiKVM issue a REST call.
enum class PowerAction {
    On,           // power on
    OffGraceful,  // ACPI/APM soft shutdown -- lets the OS shut down cleanly
    OffHard,      // immediate / forced power off
    Reset,        // hard reset / warm restart
};

// Last-known host power state, for the indicator. Stays Unknown until the backend
// reports it (ATEN has no status API, so it always reads Unknown).
enum class PowerState {
    Unknown,
    Off,
    On,
};

// Short token for logs.
inline const char* power_action_name(PowerAction action)
{
    switch (action) {
    case PowerAction::On:          return "on";
    case PowerAction::OffGraceful: return "off-graceful";
    case PowerAction::OffHard:     return "off-hard";
    case PowerAction::Reset:       return "reset";
    }
    return "?";
}

// Human label for the success toast.
inline const char* power_action_label(PowerAction action)
{
    switch (action) {
    case PowerAction::On:          return "Power On";
    case PowerAction::OffGraceful: return "Graceful Shutdown";
    case PowerAction::OffHard:     return "Force Off";
    case PowerAction::Reset:       return "Reset";
    }
    return "Power";
}

// The result of a submitted action, surfaced to the GUI as a toast. For REST
// backends `http_status` is the response code (0 if the call threw before a
// response); for ATEN (fire-and-forget, no ack) `ok` is set affirmatively on send.
struct PowerOutcome {
    PowerAction action = PowerAction::On;
    bool ok = false;
    int http_status = 0;
    std::string detail;
};

// Which operations a backend exposes, plus whether it can report status. MegaRAC and
// PiKVM support all four + status; ATEN supports the four actions but no status.
struct PowerCapabilities {
    bool query_status = false;
    bool on = false;
    bool off_graceful = false;
    bool off_hard = false;
    bool reset = false;

    bool supports(PowerAction action) const
    {
        switch (action) {
        case PowerAction::On:          return on;
        case PowerAction::OffGraceful: return off_graceful;
        case PowerAction::OffHard:     return off_hard;
        case PowerAction::Reset:       return reset;
        }
        return false;
    }

    bool any_action() const { return on || off_graceful || off_hard || reset; }
};

// Every networked BMC we support can do all four actions; only status differs.
inline PowerCapabilities default_bmc_power_caps()
{
    return PowerCapabilities{
        /*query_status=*/true,
        /*on=*/true,
        /*off_graceful=*/true,
        /*off_hard=*/true,
        /*reset=*/true,
    };
}

// Thread-safe bridge between the GUI thread (reads state, submits actions, drains
// outcomes) and the backend's network thread (publishes state + outcomes, drains
// actions). Mirrors InputQueue: actions submitted before the network installs its
// sink are queued and flushed on install. One lives in the shared ViewStateBase, so
// every backend gets it for free.
class PowerChannel {
public:
    // --- network (producer) side ---
    void publish_state(PowerState state)
    {
        state_.store(state, std::memory_order_relaxed);
    }

    void publish_outcome(PowerOutcome outcome)
    {
        std::lock_guard lock(mutex_);
        outcomes_.push_back(std::move(outcome));
    }

    // Install the backend's action handler (runs on the network thread). Flushes any
    // actions the user submitted before we connected.
    void install(std::function<void(PowerAction)> sink)
    {
        std::vector<PowerAction> pending;
        {
            std::lock_guard lock(mutex_);
            sink_ = std::move(sink);
            pending.swap(pending_);
        }
        if (sink_) {
            for (const PowerAction action : pending) {
                sink_(action);
            }
        }
    }

    void clear()
    {
        std::lock_guard lock(mutex_);
        sink_ = {};
        pending_.clear();
    }

    // --- controller (GUI) side ---
    PowerState state() const
    {
        return state_.load(std::memory_order_relaxed);
    }

    void submit(PowerAction action)
    {
        std::function<void(PowerAction)> sink;
        {
            std::lock_guard lock(mutex_);
            if (!sink_) {
                pending_.push_back(action);
                return;
            }
            sink = sink_;
        }
        sink(action);
    }

    std::vector<PowerOutcome> drain_outcomes()
    {
        std::lock_guard lock(mutex_);
        std::vector<PowerOutcome> out;
        out.swap(outcomes_);
        return out;
    }

private:
    std::atomic<PowerState> state_{PowerState::Unknown};
    std::mutex mutex_;
    std::function<void(PowerAction)> sink_;
    std::vector<PowerAction> pending_;
    std::vector<PowerOutcome> outcomes_;
};

// What the GUI talks to. Backend-agnostic: capabilities() to enable buttons, state()
// for the indicator, request() to act, take_outcomes() for toasts. All methods are
// safe to call on the GUI thread.
class PowerController {
public:
    virtual ~PowerController() = default;
    virtual PowerCapabilities capabilities() const = 0;
    virtual PowerState state() const = 0;
    virtual void request(PowerAction action) = 0;
    virtual std::vector<PowerOutcome> take_outcomes() = 0;
};

// The concrete controller every view uses: a thin adapter over the view's
// PowerChannel (which the network thread feeds). Owned by the view; outlives the GUI
// binding because the view outlives the window's frame loop.
class ViewPowerController final : public PowerController {
public:
    ViewPowerController(PowerCapabilities caps, PowerChannel& channel)
        : caps_(caps)
        , channel_(channel)
    {
    }

    PowerCapabilities capabilities() const override { return caps_; }
    PowerState state() const override { return channel_.state(); }
    std::vector<PowerOutcome> take_outcomes() override { return channel_.drain_outcomes(); }

    void request(PowerAction action) override
    {
        if (caps_.supports(action)) {
            channel_.submit(action);
        }
    }

private:
    PowerCapabilities caps_;
    PowerChannel& channel_;
};

} // namespace hitsc
