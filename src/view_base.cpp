#include "view_base.hpp"

#include "log.hpp"
#include "view_console.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace hitsc {
namespace {

void throw_view_sdl_error(const char* context)
{
    throw std::runtime_error(std::string(context) + ": " + SDL_GetError());
}

std::string message_from_exception(std::exception_ptr exception)
{
    if (!exception) {
        return {};
    }
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "Unknown error";
    }
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

void ViewStateBase::clear_exception()
{
    std::lock_guard lock(control_mutex);
    exception_ = nullptr;
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

    // Called synchronously during the OS modal move/resize loop (when the main
    // event loop is blocked), so the view reflows live as the window is dragged.
    SDL_AddEventWatch(on_event_watch, this);
}

void KvmViewBase::cleanup_sdl()
{
    if (!sdl_initialized_ && window_ == nullptr && renderer_ == nullptr) {
        return;
    }

    SDL_RemoveEventWatch(on_event_watch, this);
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
    bool close_event_logged = false;
    std::uint64_t last_status_tick = 0;

    while (running) {
        const ViewRenderState render_state = state_.view_status.render_state();
        bool render_needed = false;
        bool retry_requested = false;

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
            } else if (session_ended_) {
                // Error/disconnected console: only the retry and close keys.
                if (event.type == SDL_EVENT_KEY_DOWN) {
                    if (event.key.scancode == SDL_SCANCODE_R) {
                        retry_requested = true;
                    } else if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                        running = false;
                    }
                }
            } else if (render_state.connected) {
                // Live session — input flows even when there's no video (so the
                // user can wake a sleeping host display).
                handle_event(event, render_needed);
            } else if (event.type == SDL_EVENT_KEY_DOWN &&
                       event.key.scancode == SDL_SCANCODE_ESCAPE) {
                // Connecting console: Esc cancels.
                running = false;
            }
            have_event = SDL_PollEvent(&event);
        }

        if (retry_requested) {
            do_retry();
            first_render_ = true;
            continue;
        }

        const std::uint64_t ticks = SDL_GetTicks();

        if (visible) {
            render_frame(render_needed);
        }

        if (ticks - last_status_tick >= 1000) {
            last_status_tick = ticks;
            refresh_title();
        }

        if (network_.done() && !session_ended_) {
            session_ended_ = true;
            const std::exception_ptr exception = state_.take_exception();
            had_error_ = static_cast<bool>(exception);
            error_message_ = message_from_exception(exception);
            if (had_error_) {
                log_error() << log_name_ << " session ended with error: " << error_message_;
            } else {
                log_info() << log_name_ << " session ended";
            }
        }
    }
}

void KvmViewBase::render_frame(bool force)
{
    if (window_ == nullptr || renderer_ == nullptr) {
        return;
    }
    if ((SDL_GetWindowFlags(window_) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0) {
        return;
    }

    const ViewRenderState render_state = state_.view_status.render_state();
    ConsoleScreen screen;
    if (build_console_screen(render_state, screen)) {
        const std::uint64_t ticks = SDL_GetTicks();
        if (force || last_console_render_ == 0 || ticks - last_console_render_ >= 150) {
            last_console_render_ = ticks;
            render_view_console(renderer_, window_, screen);
        }
    } else {
        bool render_needed = force || first_render_;
        render_visible(render_needed, first_render_);
    }
}

bool KvmViewBase::build_console_screen(const ViewRenderState& render_state, ConsoleScreen& screen) const
{
    if (session_ended_) {
        if (had_error_) {
            screen.severity = ConsoleSeverity::Error;
            screen.headline = "Connection failed";
            screen.detail = error_message_;
        } else {
            screen.severity = ConsoleSeverity::Info;
            screen.headline = "Disconnected";
            screen.detail = "The session ended.";
        }
        screen.hint = "Press R to reconnect      Esc to close";
        return true;
    }
    if (!render_state.connected) {
        screen.severity = ConsoleSeverity::Info;
        screen.headline = "Connecting to " + host_ + "...";
        screen.hint = "Esc to cancel";
        return true;
    }
    if (render_state.display_online.has_value() && !render_state.display_online.value()) {
        screen.severity = ConsoleSeverity::Info;
        screen.headline = "No video signal";
        screen.detail = "The host display may be off or asleep.";
        return true;
    }
    return false;
}

bool SDLCALL KvmViewBase::on_event_watch(void* userdata, SDL_Event* event)
{
    if (event != nullptr &&
        (event->type == SDL_EVENT_WINDOW_RESIZED ||
         event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
         event->type == SDL_EVENT_WINDOW_EXPOSED)) {
        static_cast<KvmViewBase*>(userdata)->render_frame(true);
    }
    return true;
}

void KvmViewBase::do_retry()
{
    log_info() << log_name_ << " reconnect requested";
    network_.stop();
    network_started_ = false;
    state_.clear_exception();
    reset_for_reconnect();
    state_.view_status.kvm_connection(false);
    session_ended_ = false;
    had_error_ = false;
    error_message_.clear();
    start_network(network_);
    network_started_ = true;
}

} // namespace hitsc
