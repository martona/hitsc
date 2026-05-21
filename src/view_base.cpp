#include "view_base.hpp"

#include "log.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hitsc {
namespace {

void throw_view_sdl_error(const char* context)
{
    throw std::runtime_error(std::string(context) + ": " + SDL_GetError());
}

} // namespace

void ViewStateBase::set_exception(std::exception_ptr exception)
{
    std::lock_guard lock(control_mutex);
    if (!exception_) {
        exception_ = exception;
    }
}

std::exception_ptr ViewStateBase::take_exception()
{
    std::lock_guard lock(control_mutex);
    return exception_;
}

void ViewStateBase::set_force_close(std::function<void()> force_close)
{
    std::lock_guard lock(control_mutex);
    force_close_ = std::move(force_close);
}

std::function<void()> ViewStateBase::force_close_snapshot()
{
    std::lock_guard lock(control_mutex);
    return force_close_;
}

void ViewStateBase::set_frame_event_type(Uint32 frame_event_type)
{
    frame_event_type_.store(frame_event_type);
}

bool ViewStateBase::is_frame_event(Uint32 event_type) const
{
    const auto frame_event_type = static_cast<Uint32>(frame_event_type_.load());
    return frame_event_type != 0 && event_type == frame_event_type;
}

void ViewStateBase::clear_frame_event_pending()
{
    frame_event_pending_.store(false);
}

void ViewStateBase::push_render_event()
{
    const auto frame_event_type = static_cast<Uint32>(frame_event_type_.load());
    if (frame_event_type != 0 && !frame_event_pending_.exchange(true)) {
        SDL_Event event{};
        event.type = frame_event_type;
        if (!SDL_PushEvent(&event)) {
            frame_event_pending_.store(false);
        }
    }
}

KvmNetworkWorker::KvmNetworkWorker(ViewStateBase& state, std::function<void()> cleanup)
    : state_(state)
    , cleanup_(std::move(cleanup))
{
}

void KvmNetworkWorker::stop()
{
    stop_requested_.store(true);

    if (std::function<void()> force_close = state_.force_close_snapshot()) {
        force_close();
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

bool KvmNetworkWorker::done() const
{
    return done_.load();
}

KvmViewBase::KvmViewBase(
    ViewStateBase& state,
    std::string host,
    std::string log_name,
    std::function<void()> network_cleanup)
    : state_(state)
    , network_(state_, std::move(network_cleanup))
    , host_(std::move(host))
    , log_name_(std::move(log_name))
{
}

void KvmViewBase::run()
{
    try {
        initialize_sdl();
        start_network(network_);
        network_started_ = true;
        event_loop();

        if (window_ != nullptr) {
            SDL_HideWindow(window_);
        }
        network_.stop();
        network_started_ = false;
        cleanup_sdl();
    } catch (...) {
        if (network_started_) {
            network_.stop();
            network_started_ = false;
        }
        cleanup_sdl();
        throw;
    }
}

SDL_Window* KvmViewBase::window() const
{
    return window_;
}

SDL_Renderer* KvmViewBase::renderer() const
{
    return renderer_;
}

SDL_FRect KvmViewBase::centered_target_rect(
    int window_width,
    int window_height,
    int frame_width,
    int frame_height)
{
    const float width_scale = static_cast<float>(window_width) / static_cast<float>(frame_width);
    const float height_scale = static_cast<float>(window_height) / static_cast<float>(frame_height);
    const float scale = std::min(width_scale, height_scale);
    SDL_FRect rect{};
    rect.w = std::floor(static_cast<float>(frame_width) * scale);
    rect.h = std::floor(static_cast<float>(frame_height) * scale);
    rect.x = std::floor((static_cast<float>(window_width) - rect.w) / 2.0f);
    rect.y = std::floor((static_cast<float>(window_height) - rect.h) / 2.0f);
    return rect;
}

SDL_FRect KvmViewBase::current_target_rect(SDL_Window* window, int frame_width, int frame_height)
{
    int window_width = 0;
    int window_height = 0;
    if (!SDL_GetWindowSizeInPixels(window, &window_width, &window_height)) {
        SDL_GetWindowSize(window, &window_width, &window_height);
    }
    return centered_target_rect(window_width, window_height, frame_width, frame_height);
}

SDL_FRect KvmViewBase::current_target_rect(int frame_width, int frame_height) const
{
    return current_target_rect(window_, frame_width, frame_height);
}

void KvmViewBase::clear_background() const
{
    SDL_SetRenderDrawColor(renderer_, 12, 14, 18, 255);
    SDL_RenderClear(renderer_);
}

void KvmViewBase::present() const
{
    SDL_RenderPresent(renderer_);
}

void KvmViewBase::frame_presented(int width, int height)
{
    state_.view_status.frame_presented(width, height);
}

void KvmViewBase::refresh_title()
{
    if (window_ != nullptr) {
        SDL_SetWindowTitle(window_, state_.view_status.title(host_).c_str());
    }
}

SDL_Renderer* KvmViewBase::create_renderer(SDL_Window* window)
{
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        throw_view_sdl_error("SDL_CreateRenderer");
    }
    return renderer;
}

void KvmViewBase::destroy_renderer(SDL_Renderer* renderer)
{
    SDL_DestroyRenderer(renderer);
}

void KvmViewBase::initialize_sdl()
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw_view_sdl_error("SDL_Init");
    }
    sdl_initialized_ = true;

    const Uint32 frame_event_type = SDL_RegisterEvents(1);
    if (frame_event_type == 0) {
        throw_view_sdl_error("SDL_RegisterEvents");
    }
    state_.set_frame_event_type(frame_event_type);

    window_ = SDL_CreateWindow("hitsc", 1024, 768, SDL_WINDOW_RESIZABLE);
    if (window_ == nullptr) {
        throw_view_sdl_error("SDL_CreateWindow");
    }
    refresh_title();

    renderer_ = create_renderer(window_);
}

void KvmViewBase::cleanup_sdl()
{
    if (!sdl_initialized_ && window_ == nullptr && renderer_ == nullptr) {
        return;
    }

    SDL_CaptureMouse(false);
    before_sdl_cleanup();

    if (renderer_ != nullptr) {
        destroy_renderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    if (sdl_initialized_) {
        SDL_Quit();
        sdl_initialized_ = false;
    }
}

void KvmViewBase::event_loop()
{
    bool running = true;
    bool visible = true;
    bool first_render = true;
    bool close_event_logged = false;
    std::uint64_t last_status_tick = 0;

    while (running) {
        bool render_needed = first_render;
        SDL_Event event{};
        bool have_event = SDL_WaitEventTimeout(&event, 16);
        while (have_event) {
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                if (!close_event_logged) {
                    close_event_logged = true;
                    log_info() << log_name_ << " window close event"
                               << " type=" << event.type;
                }
                on_close();
                running = false;
            } else if (event.type == SDL_EVENT_WINDOW_MINIMIZED ||
                       event.type == SDL_EVENT_WINDOW_HIDDEN) {
                visible = false;
                state_.clear_frame_event_pending();
                state_.view_status.minimize();
                on_minimized();
                last_status_tick = 0;
                refresh_title();
            } else if (event.type == SDL_EVENT_WINDOW_RESTORED ||
                       event.type == SDL_EVENT_WINDOW_SHOWN) {
                visible = true;
                on_restored();
                render_needed = true;
                last_status_tick = 0;
                refresh_title();
            } else if (state_.is_frame_event(event.type)) {
                state_.clear_frame_event_pending();
                render_needed = true;
            } else if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                       event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                       event.type == SDL_EVENT_WINDOW_EXPOSED) {
                render_needed = true;
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                on_focus_lost();
            } else {
                handle_event(event, render_needed);
            }
            have_event = SDL_PollEvent(&event);
        }

        if (visible) {
            render_visible(render_needed, first_render);
        }

        const std::uint64_t ticks = SDL_GetTicks();
        if (ticks - last_status_tick >= 1000) {
            last_status_tick = ticks;
            refresh_title();
        }

        if (network_.done()) {
            if (std::exception_ptr exception = state_.take_exception()) {
                std::rethrow_exception(exception);
            }
            running = false;
        }
    }
}

} // namespace hitsc
