#pragma once

#include "view_status.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace hitsc {

class ViewStateBase {
public:
    void set_exception(std::exception_ptr exception);
    std::exception_ptr take_exception();

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
        std::string log_name,
        std::function<void()> network_cleanup);
    virtual ~KvmViewBase() = default;

    KvmViewBase(const KvmViewBase&) = delete;
    KvmViewBase& operator=(const KvmViewBase&) = delete;

    void run();

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
    virtual void handle_event(const SDL_Event& event, bool& render_needed) = 0;
    virtual void render_visible(bool& render_needed, bool& first_render) = 0;

private:
    void initialize_sdl();
    void cleanup_sdl();
    void event_loop();

    ViewStateBase& state_;
    KvmNetworkWorker network_;
    std::string host_;
    std::string log_name_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool sdl_initialized_ = false;
    bool network_started_ = false;
};

} // namespace hitsc
