#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
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

// What the GUI talks to. Backend-agnostic: state() drives the title-bar glyph, mount/unmount
// act, take_outcomes() feeds toasts. Owned by the view (outlives the GUI binding). All methods
// are safe to call on the GUI thread.
class VirtualMediaController {
public:
    virtual ~VirtualMediaController() = default;
    virtual MediaState state() const = 0;
    virtual void mount(std::string iso_path) = 0;
    virtual void unmount() = 0;
    virtual std::vector<MediaOutcome> take_outcomes() = 0;
};

// The concrete controller a view uses: a thin adapter over the view's MediaChannel (state +
// outcomes, fed by the media network thread) plus mount/unmount callbacks the view supplies
// (which start/stop its media worker thread). Owned by the view.
class ViewVirtualMediaController final : public VirtualMediaController {
public:
    ViewVirtualMediaController(
        MediaChannel& channel,
        std::function<void(std::string)> mount,
        std::function<void()> unmount)
        : channel_(channel)
        , mount_(std::move(mount))
        , unmount_(std::move(unmount))
    {
    }

    MediaState state() const override { return channel_.state(); }
    std::vector<MediaOutcome> take_outcomes() override { return channel_.drain_outcomes(); }

    void mount(std::string iso_path) override
    {
        if (mount_) {
            mount_(std::move(iso_path));
        }
    }

    void unmount() override
    {
        if (unmount_) {
            unmount_();
        }
    }

private:
    MediaChannel& channel_;
    std::function<void(std::string)> mount_;
    std::function<void()> unmount_;
};

} // namespace hitsc
