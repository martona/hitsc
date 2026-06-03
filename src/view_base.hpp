#pragma once

#include "view_console.hpp"
#include "view_input_types.hpp"
#include "view_status.hpp"

#include <SDL3/SDL.h>

#include <QImage>

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

namespace hitsc {

class KvmInputController;

struct ViewWindow {
    SDL_Window* window = nullptr;
    Uint32 frame_event_type = 0;
};

// Initialize SDL video, register the per-frame event, and create the view
// window. Throws on failure (tearing SDL back down if it had been initialized).
// geometry_key, when non-empty, restores the window's saved per-host position
// and size (the same key cleanup uses to save it back).
ViewWindow make_view_window(const std::string& geometry_key = "");

// Persist a window's current position/size under its per-host geometry key.
// No-op for an empty key or a minimized/maximized window. Used by the views on
// cleanup and by the auto bootstrap when it closes the window before handoff.
void save_view_window_geometry(SDL_Window* window, const std::string& geometry_key);

class ViewStateBase {
public:
    void set_exception(std::exception_ptr exception);
    std::exception_ptr take_exception();
    void clear_exception();

    void set_force_close(std::function<void()> force_close);
    std::function<void()> force_close_snapshot();

    void set_frame_event_type(Uint32 frame_event_type);
    bool is_frame_event(Uint32 event_type) const;
    void clear_frame_event_pending();
    void push_render_event();

    std::mutex control_mutex;
    ViewStatus view_status;

private:
    std::exception_ptr exception_;
    std::function<void()> force_close_;
    std::atomic_uint32_t frame_event_type_{0};
    std::atomic_bool frame_event_pending_{false};
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

class KvmViewBase {
public:
    KvmViewBase(
        ViewStateBase& state,
        std::string host,
        std::string geometry_key,
        std::string log_name,
        std::function<void()> network_cleanup);
    virtual ~KvmViewBase() = default;

    KvmViewBase(const KvmViewBase&) = delete;
    KvmViewBase& operator=(const KvmViewBase&) = delete;

    void run();

    // Adopt a pre-created window/SDL context (the auto-detect bootstrap creates
    // the window before the backend is known) instead of creating one. The view
    // then owns SDL teardown. Call before run().
    void adopt_sdl(const ViewWindow& view_window);

    // -----------------------------------------------------------------------
    // Qt-hosted mode (step 5 drives these instead of run()). None of these
    // create or touch an SDL window/renderer; the run()/event_loop() SDL path
    // below is untouched and remains the default until the migration completes.
    // -----------------------------------------------------------------------
    void hosted_start_network();  // start the network worker (no SDL window)
    void hosted_stop_network();   // stop it (idempotent)
    void hosted_poll();           // per-tick: detect session end, record error
    bool hosted_session_ended() const { return session_ended_; }
    bool hosted_connected() const;  // live KVM session (render_state.connected)
    void hosted_retry();          // 'R' on the disconnected console

    std::string hosted_title() const;

    // What the surface should display now: a console screen, or nullopt meaning
    // "show the latest video frame" via latest_frame_image().
    std::optional<ConsoleScreen> hosted_console_screen() const;
    virtual std::optional<QImage> latest_frame_image() { return std::nullopt; }
    // Cheap current-frame dimensions for hosted pointer mapping (no conversion).
    virtual std::optional<std::pair<int, int>> latest_frame_size() { return std::nullopt; }

    // Lifecycle, mirroring the SDL event_loop's window-event branches.
    void hosted_minimized();
    void hosted_restored();
    void hosted_focus_lost();
    void hosted_close();

    // SDL-free input feed (Qt surface/window -> the backend's controller).
    void feed_key(const KvmKeyEvent& key);
    void feed_pointer_button(const KvmPointerButton& button);
    void feed_pointer_motion(const KvmPointerMotion& motion);
    void feed_pointer_wheel(const KvmPointerWheel& wheel);

    // The Qt surface reports its current (logical) size so hosted pointer mapping
    // can centre the frame within it (there is no SDL window to query).
    void hosted_set_surface_size(int width, int height);

protected:
    SDL_Window* window() const;
    SDL_Renderer* renderer() const;

    static SDL_FRect centered_target_rect(
        int window_width,
        int window_height,
        int frame_width,
        int frame_height);
    static SDL_FRect current_target_rect(SDL_Window* window, int frame_width, int frame_height);

    SDL_FRect current_target_rect(int frame_width, int frame_height) const;
    void clear_background() const;
    void present() const;
    void frame_presented(int width, int height);
    void refresh_title();

    virtual SDL_Renderer* create_renderer(SDL_Window* window);
    virtual void destroy_renderer(SDL_Renderer* renderer);
    virtual void before_sdl_cleanup() = 0;

    virtual void start_network(KvmNetworkWorker& network) = 0;

    virtual void on_close() {};
    virtual void on_minimized() = 0;
    virtual void on_restored() = 0;
    virtual void on_focus_lost() = 0;

    // Drop any stale per-session state (frames, decoder, queues) so a
    // user-initiated reconnect starts clean. Default is a no-op.
    virtual void reset_for_reconnect() {}

    virtual void handle_event(const SDL_Event& event, bool& render_needed) = 0;
    virtual void render_visible(bool& render_needed, bool& first_render) = 0;

    // Qt-hosted input routing: a backend returns its KvmInputController so the
    // base's feed_*() can drive it. Default null = no hosted input.
    virtual KvmInputController* hosted_input_controller() { return nullptr; }

private:
    void initialize_sdl();
    void cleanup_sdl();
    void event_loop();
    void do_retry();
    void render_frame(bool force);
    bool build_console_screen(const ViewRenderState& render_state, ConsoleScreen& screen) const;
    static bool SDLCALL on_event_watch(void* userdata, SDL_Event* event);

    ViewStateBase& state_;
    KvmNetworkWorker network_;
    std::string host_;
    std::string geometry_key_;
    std::string log_name_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool sdl_initialized_ = false;
    bool network_started_ = false;
    bool adopted_ = false;
    SDL_Window* adopted_window_ = nullptr;
    Uint32 adopted_frame_event_type_ = 0;
    bool first_render_ = true;
    bool session_ended_ = false;
    bool had_error_ = false;
    std::string error_message_;
    std::uint64_t last_console_render_ = 0;
    int hosted_surface_w_ = 0;
    int hosted_surface_h_ = 0;
};

} // namespace hitsc
