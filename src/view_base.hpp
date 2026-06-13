#pragma once

#include "console_screen.hpp"
#include "power_control.hpp"
#include "view_input_types.hpp"
#include "view_status.hpp"

#include <QImage>
#include <QRect>

#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hitsc {

class KvmInputController;

class ViewStateBase {
public:
    void set_exception(std::exception_ptr exception);
    std::exception_ptr take_exception();
    void clear_exception();

    void set_force_close(std::function<void()> force_close);
    std::function<void()> force_close_snapshot();

    std::mutex control_mutex;
    ViewStatus view_status;
    // Power display/control bridge (GUI <-> network thread). Fed by the backend's
    // network session; read/driven by the title-bar power widget via PowerController.
    PowerChannel power;

private:
    std::exception_ptr exception_;
    std::function<void()> force_close_;
};

template <typename T>
class LatestMailbox {
public:
    std::shared_ptr<const T> publish(T value)
    {
        std::lock_guard lock(mutex_);
        value.sequence = ++sequence_;
        latest_ = std::make_shared<T>(std::move(value));
        return latest_;
    }

    std::shared_ptr<const T> latest(std::uint64_t last_sequence)
    {
        std::lock_guard lock(mutex_);
        if (!latest_ || latest_->sequence == last_sequence) {
            return {};
        }
        return latest_;
    }

    std::shared_ptr<const T> clear()
    {
        std::shared_ptr<const T> latest;
        std::lock_guard lock(mutex_);
        latest = std::move(latest_);
        return latest;
    }

private:
    std::mutex mutex_;
    std::shared_ptr<const T> latest_;
    std::uint64_t sequence_ = 0;
};

// Ordered, multi-slot counterpart to LatestMailbox for DIFFERENTIAL video
// streams (ASPEED dual-JPEG): each frame is a delta on its predecessor — SKIP
// blocks reuse the prior frame's pixels and PASS2 blocks accumulate onto its
// stored YUV — so the consumer must decode EVERY frame, in arrival order.
// Dropping any one (as LatestMailbox does) corrupts the chain until the next
// keyframe; that is what garbled fast/small hosts whose frames outran the view
// tick. Use LatestMailbox for already-decoded frames or non-differential data
// where only the newest matters.
template <typename T>
class FrameQueue {
public:
    void publish(T value)
    {
        std::lock_guard lock(mutex_);
        value.sequence = ++sequence_;
        queue_.push_back(std::make_shared<const T>(std::move(value)));
        if (queue_.size() > kMaxQueued) {
            // The consumer has fallen catastrophically behind. We cannot silently
            // skip differential frames, so drop the oldest and flag that a full
            // keyframe refresh is needed to resync.
            queue_.pop_front();
            overflowed_ = true;
        }
    }

    // Every frame published since the previous drain, in arrival order.
    std::vector<std::shared_ptr<const T>> drain()
    {
        std::lock_guard lock(mutex_);
        std::vector<std::shared_ptr<const T>> out(queue_.begin(), queue_.end());
        queue_.clear();
        return out;
    }

    // True (once) if frames were dropped on overflow since the last check.
    bool overflowed()
    {
        std::lock_guard lock(mutex_);
        const bool overflowed = overflowed_;
        overflowed_ = false;
        return overflowed;
    }

    void clear()
    {
        std::lock_guard lock(mutex_);
        queue_.clear();
        overflowed_ = false;
    }

private:
    // Bounds memory if the view thread stalls (e.g. a long modal). Large enough
    // that normal bursts (a few frames per tick) never trip it.
    static constexpr std::size_t kMaxQueued = 240;

    std::mutex mutex_;
    std::deque<std::shared_ptr<const T>> queue_;
    std::uint64_t sequence_ = 0;
    bool overflowed_ = false;
};

template <typename Work>
class InputQueue {
public:
    void enqueue(Work work)
    {
        std::function<void(Work)> sink;
        {
            std::lock_guard lock(mutex_);
            sink = sink_;
            if (!sink) {
                pending_.push_back(std::move(work));
                return;
            }
        }

        sink(std::move(work));
    }

    void install(std::function<void(Work)> sink)
    {
        std::deque<Work> pending;
        {
            std::lock_guard lock(mutex_);
            sink_ = sink;
            pending.swap(pending_);
        }

        for (Work& work : pending) {
            sink(std::move(work));
        }
    }

    void clear()
    {
        std::lock_guard lock(mutex_);
        sink_ = {};
        pending_.clear();
    }

private:
    std::mutex mutex_;
    std::function<void(Work)> sink_;
    std::deque<Work> pending_;
};

class KvmNetworkWorker {
public:
    KvmNetworkWorker(ViewStateBase& state, std::function<void()> cleanup);

    KvmNetworkWorker(const KvmNetworkWorker&) = delete;
    KvmNetworkWorker& operator=(const KvmNetworkWorker&) = delete;

    template <typename Run>
    void start(Run run)
    {
        stop_requested_.store(false);
        done_.store(false);
        thread_ = std::thread([this, run = std::move(run)]() mutable {
            try {
                run(stop_requested_);
            } catch (...) {
                state_.set_exception(std::current_exception());
            }
            if (cleanup_) {
                cleanup_();
            }
            state_.set_force_close({});
            done_.store(true);
        });
    }

    void stop();
    bool done() const;

private:
    ViewStateBase& state_;
    std::function<void()> cleanup_;
    std::atomic_bool stop_requested_{false};
    std::atomic_bool done_{false};
    std::thread thread_;
};

// A cursor sprite to overlay on the video, in base-frame pixel coordinates. The
// software backends (ASPEED hardware cursor) build it; the surface draws it as a
// SEPARATE blended GPU quad so cursor motion never re-uploads the (large) base
// frame. A null/empty sprite means "hide the cursor".
struct CursorOverlay {
    QImage sprite;  // RGBA8888, straight (non-premultiplied) alpha; empty => hide
    int x = 0;      // sprite top-left, in base-frame pixels
    int y = 0;
};

// One view tick's software output. Each field is independent:
//   base   set => a newly composited base frame to upload (nullopt => unchanged)
//   cursor set => the cursor moved / changed shape / was hidden (nullopt => unchanged)
// The surface retains whichever field is absent. latest_frame() returns nullopt
// outright when BOTH are unchanged, so an idle stream re-draws existing textures
// for free (no convert, no upload).
struct SoftwareFrame {
    std::optional<QImage> base;
    std::optional<CursorOverlay> cursor;
    // When base is set: the sub-region of base that changed, so the surface can
    // patch only that rectangle into its persistent texture. Absent => upload the
    // whole base (first frame, resolution change, or a resync after frames dropped).
    std::optional<QRect> dirty;
};

class KvmViewBase {
public:
    KvmViewBase(
        ViewStateBase& state,
        std::string host,
        std::string log_name,
        std::function<void()> network_cleanup);
    virtual ~KvmViewBase() = default;

    KvmViewBase(const KvmViewBase&) = delete;
    KvmViewBase& operator=(const KvmViewBase&) = delete;

    // -----------------------------------------------------------------------
    // Qt-native entry. The Qt viewer host (run_viewer) drives these.
    // -----------------------------------------------------------------------
    void hosted_start_network();  // start the network worker
    void hosted_stop_network();   // stop it (idempotent)
    void hosted_poll();           // per-tick: detect session end, record error
    bool hosted_session_ended() const { return session_ended_; }
    bool hosted_connected() const;  // live KVM session (render_state.connected)
    void hosted_retry();          // 'R' on the disconnected console

    std::string hosted_title() const;

    // What the surface should display now: a console screen, or nullopt meaning
    // "show the latest video frame" via latest_frame().
    std::optional<ConsoleScreen> hosted_console_screen() const;
    virtual std::optional<SoftwareFrame> latest_frame() { return std::nullopt; }
    // Cheap current-frame dimensions for hosted pointer mapping (no conversion).
    virtual std::optional<std::pair<int, int>> latest_frame_size() { return std::nullopt; }
    // Resolution to assume for pointer mapping when NO video frame has decoded yet
    // (e.g. the host display is asleep at connect). Lets a mouse move still map and
    // reach the host, waking it -- the way the JS client does. latest_frame_size()
    // overrides this the instant real video arrives. Default: none (mouse stays gated).
    virtual std::optional<std::pair<int, int>> hosted_input_resolution() { return std::nullopt; }

    // Power display/control for this session, or nullptr if unsupported. The viewer
    // host binds the title-bar power widget to it; the GUI uses only this interface.
    virtual PowerController* power_controller() { return nullptr; }

    // True if this backend supports virtual-media (CD/ISO) redirection. The viewer host
    // shows the title-bar "mount CD" control only when true. Default: unsupported. (For
    // now a capability flag; this becomes a VirtualMediaController* -- mirroring
    // power_controller() -- once the IUSB /cd-server transport lands.)
    virtual bool hosted_supports_virtual_media() const { return false; }

    // Zero-copy hardware path (pikvm D3D11VA). The surface hands its QRhi
    // ID3D11Device via set_rhi_d3d11_device once RHI is up; latest_hardware_frame
    // then yields decoded NV12 textures to import. Default: software-only.
    virtual std::optional<HardwareVideoFrame> latest_hardware_frame() { return std::nullopt; }
    virtual void set_rhi_d3d11_device(void* d3d11_device) { (void)d3d11_device; }

    // Window lifecycle.
    void hosted_minimized();
    void hosted_restored();
    void hosted_focus_lost();
    void hosted_close();

    // Input feed (Qt surface/window -> the backend's controller).
    void feed_key(const KvmKeyEvent& key);
    void feed_pointer_button(const KvmPointerButton& button);
    void feed_pointer_motion(const KvmPointerMotion& motion);
    void feed_pointer_wheel(const KvmPointerWheel& wheel);

    // The Qt surface reports its current (logical) size so hosted pointer mapping
    // can centre the frame within it.
    void hosted_set_surface_size(int width, int height);

protected:
    // Aspect-fit a frame within a surface of the given size (used for hosted
    // pointer mapping).
    static TargetRect centered_target_rect(
        int window_width,
        int window_height,
        int frame_width,
        int frame_height);

    void frame_presented(int width, int height);

    virtual void start_network(KvmNetworkWorker& network) = 0;

    virtual void on_close() {};
    virtual void on_minimized() = 0;
    virtual void on_restored() = 0;
    virtual void on_focus_lost() = 0;

    // Drop any stale per-session state (frames, decoder, queues) so a
    // user-initiated reconnect starts clean. Default is a no-op.
    virtual void reset_for_reconnect() {}

    // Qt-hosted input routing: a backend returns its KvmInputController so the
    // base's feed_*() can drive it. Default null = no hosted input.
    virtual KvmInputController* hosted_input_controller() { return nullptr; }

private:
    void do_retry();
    bool build_console_screen(const ViewRenderState& render_state, ConsoleScreen& screen) const;

    ViewStateBase& state_;
    KvmNetworkWorker network_;
    std::string host_;
    std::string log_name_;
    bool network_started_ = false;
    bool session_ended_ = false;
    bool had_error_ = false;
    std::string error_message_;
    int hosted_surface_w_ = 0;
    int hosted_surface_h_ = 0;
};

} // namespace hitsc
