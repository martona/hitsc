#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace hitsc {

// Where a virtual-media mount is in its lifecycle, for the title-bar CD control.
enum class MediaState {
    Idle,      // nothing mounted
    Mounting,  // connecting / authenticating the redirection
    Mounted,   // the BMC accepted the virtual CD and is serving reads
    Error,     // the last mount attempt failed (detail in the outcomes)
};

// A surfaced result of a mount/unmount attempt, shown to the user as a toast.
struct MediaOutcome {
    bool ok = false;
    std::string detail;
};

// Thread-safe feedback bridge between the media network thread (publishes state + outcomes)
// and the GUI thread (reads state, drains outcomes). The mount/unmount *commands* are not
// carried here -- they start/stop the media worker directly -- so unlike PowerChannel this is
// feedback-only. Lives in the media session state the view owns.
class MediaChannel {
public:
    // --- network (producer) side ---
    void publish_state(MediaState state) { state_.store(state, std::memory_order_relaxed); }

    void publish_outcome(MediaOutcome outcome)
    {
        std::lock_guard lock(mutex_);
        outcomes_.push_back(std::move(outcome));
    }

    // --- GUI (consumer) side ---
    MediaState state() const { return state_.load(std::memory_order_relaxed); }

    std::vector<MediaOutcome> drain_outcomes()
    {
        std::lock_guard lock(mutex_);
        std::vector<MediaOutcome> out;
        out.swap(outcomes_);
        return out;
    }

private:
    std::atomic<MediaState> state_{MediaState::Idle};
    std::mutex mutex_;
    std::vector<MediaOutcome> outcomes_;
};

} // namespace hitsc
