#include "pikvm_view.hpp"

#include "diagnostics.hpp"
#include "log.hpp"
#include "pikvm_events.hpp"
#include "pikvm_session.hpp"
#include "pikvm_video.hpp"

#include <SDL3/SDL.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace hitsc {
namespace asio = boost::asio;
namespace ssl = asio::ssl;

namespace {

struct PikvmViewState {
    std::mutex frame_mutex;
    std::mutex control_mutex;
    std::shared_ptr<const PikvmVideoFrame> frame;
    std::uint64_t frame_sequence = 0;
    std::string status = "starting";
    std::exception_ptr exception;
    std::function<void()> force_close;
};

void throw_sdl_error(std::string_view context)
{
    throw std::runtime_error(std::string(context) + ": " + SDL_GetError());
}

SDL_FRect centered_target_rect(int window_width, int window_height, int frame_width, int frame_height)
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

SDL_FRect current_target_rect(SDL_Window* window, int frame_width, int frame_height)
{
    int window_width = 0;
    int window_height = 0;
    if (!SDL_GetWindowSizeInPixels(window, &window_width, &window_height)) {
        SDL_GetWindowSize(window, &window_width, &window_height);
    }
    return centered_target_rect(window_width, window_height, frame_width, frame_height);
}

int sampled_average_rgb(const std::vector<std::uint8_t>& rgba)
{
    if (rgba.empty()) {
        return 0;
    }

    std::uint64_t total = 0;
    std::uint64_t samples = 0;
    constexpr std::size_t stride_pixels = 257;
    for (std::size_t pixel = 0; pixel * 4 + 2 < rgba.size(); pixel += stride_pixels) {
        const std::size_t offset = pixel * 4;
        total += rgba[offset] + rgba[offset + 1] + rgba[offset + 2];
        ++samples;
    }
    return samples == 0 ? 0 : static_cast<int>(total / (samples * 3));
}

void set_pikvm_status(PikvmViewState& state, std::string status)
{
    std::lock_guard lock(state.control_mutex);
    state.status = std::move(status);
}

std::string pikvm_status_snapshot(PikvmViewState& state)
{
    std::lock_guard lock(state.control_mutex);
    return state.status;
}

void set_pikvm_exception(PikvmViewState& state, std::exception_ptr exception)
{
    std::lock_guard lock(state.control_mutex);
    if (!state.exception) {
        state.exception = exception;
    }
}

std::exception_ptr take_pikvm_exception(PikvmViewState& state)
{
    std::lock_guard lock(state.control_mutex);
    return state.exception;
}

void set_pikvm_force_close(PikvmViewState& state, std::function<void()> force_close)
{
    std::lock_guard lock(state.control_mutex);
    state.force_close = std::move(force_close);
}

void store_pikvm_frame(PikvmViewState& state, PikvmVideoFrame frame)
{
    std::lock_guard lock(state.frame_mutex);
    frame.sequence = ++state.frame_sequence;
    state.frame = std::make_shared<PikvmVideoFrame>(std::move(frame));
}

std::shared_ptr<const PikvmVideoFrame> take_latest_pikvm_frame(
    PikvmViewState& state,
    std::uint64_t last_sequence)
{
    std::lock_guard lock(state.frame_mutex);
    if (!state.frame || state.frame->sequence == last_sequence) {
        return {};
    }
    return state.frame;
}

void force_close_weak_socket(std::weak_ptr<PikvmWebSocket> weak_ws)
{
    if (std::shared_ptr<PikvmWebSocket> ws = weak_ws.lock()) {
        force_close_pikvm_websocket(*ws);
    }
}

void run_pikvm_network_session(
    const PikvmViewOptions& options,
    PikvmViewState& state,
    std::atomic_bool& stop_requested)
{
    set_pikvm_status(state, "logging in");
    PikvmSession session = login_pikvm(options.login);
    PikvmLogoutGuard logout_guard(options.login);
    logout_guard.arm(session);
    log_info() << "pikvm login succeeded";
    if (options.login.verbose) {
        log_info() << "cookies stored: " << session.cookies.size();
    }

    if (stop_requested.load()) {
        return;
    }

    asio::io_context io;
    ssl::context tls_context(ssl::context::tls_client);

    std::weak_ptr<PikvmWebSocket> weak_event_ws;
    std::weak_ptr<PikvmWebSocket> weak_video_ws;
    auto request_stop = [&] {
        stop_requested.store(true);
        force_close_weak_socket(weak_event_ws);
        force_close_weak_socket(weak_video_ws);
        io.stop();
    };

    set_pikvm_status(state, "connecting event websocket");
    std::shared_ptr<PikvmWebSocket> event_ws =
        connect_pikvm_websocket(
            io,
            tls_context,
            options.login,
            session.cookies,
            options.idle_timeout_seconds,
            "/api/ws?stream=1");
    weak_event_ws = event_ws;
    if (stop_requested.load()) {
        request_stop();
        return;
    }

    set_pikvm_status(state, "connecting video websocket");
    std::shared_ptr<PikvmWebSocket> video_ws =
        connect_pikvm_websocket(
            io,
            tls_context,
            options.login,
            session.cookies,
            options.idle_timeout_seconds,
            "/api/media/ws");
    weak_video_ws = video_ws;
    set_pikvm_force_close(state, request_stop);

    auto on_error = [&](std::exception_ptr exception) {
        set_pikvm_exception(state, exception);
        request_stop();
    };

    set_pikvm_status(state, "connected");
    start_pikvm_event_drain(event_ws, options, stop_requested, on_error);
    start_pikvm_video_stream(
        video_ws,
        options,
        stop_requested,
        [&](PikvmVideoFrame frame) {
            store_pikvm_frame(state, std::move(frame));
        },
        on_error);

    io.run();
    set_pikvm_force_close(state, {});
    if (stop_requested.load()) {
        set_pikvm_status(state, "stopped");
    }
}

void stop_pikvm_network(
    PikvmViewState& state,
    std::atomic_bool& stop_requested,
    std::thread& network_thread,
    bool verbose)
{
    if (verbose) {
        log_debug() << "pikvm network stop begin";
    }

    stop_requested.store(true);
    std::function<void()> force_close;
    {
        std::lock_guard lock(state.control_mutex);
        force_close = state.force_close;
    }
    if (force_close) {
        force_close();
    }

    if (network_thread.joinable()) {
        network_thread.join();
    }

    if (verbose) {
        log_debug() << "pikvm network stop end";
    }
}

} // namespace

void run_pikvm_view(const PikvmViewOptions& options)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw_sdl_error("SDL_Init");
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    auto state = std::make_shared<PikvmViewState>();
    auto stop_requested = std::make_shared<std::atomic_bool>(false);
    auto network_done = std::make_shared<std::atomic_bool>(false);
    std::thread network_thread;

    try {
        window = SDL_CreateWindow("hitsc - PiKVM", 1024, 768, SDL_WINDOW_RESIZABLE);
        if (window == nullptr) {
            throw_sdl_error("SDL_CreateWindow");
        }

        renderer = SDL_CreateRenderer(window, nullptr);
        if (renderer == nullptr) {
            throw_sdl_error("SDL_CreateRenderer");
        }

        PikvmViewOptions network_options = options;
        network_thread = std::thread([network_options, state, stop_requested, network_done] {
            try {
                run_pikvm_network_session(network_options, *state, *stop_requested);
            } catch (...) {
                set_pikvm_exception(*state, std::current_exception());
            }
            set_pikvm_force_close(*state, {});
            network_done->store(true);
        });

        bool running = true;
        bool first_render = true;
        bool close_event_logged = false;
        std::uint64_t last_sequence = 0;
        std::uint64_t last_status_tick = 0;
        int texture_width = 0;
        int texture_height = 0;
        int presented_frames = 0;

        while (running) {
            bool render_needed = first_render;
            SDL_Event event{};
            bool have_event = SDL_WaitEventTimeout(&event, 16);
            while (have_event) {
                if (event.type == SDL_EVENT_QUIT ||
                    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    if (!close_event_logged) {
                        close_event_logged = true;
                        log_info() << "pikvm window close event"
                                   << " type=" << event.type;
                    }
                    running = false;
                } else if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                           event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                           event.type == SDL_EVENT_WINDOW_EXPOSED) {
                    render_needed = true;
                }
                have_event = SDL_PollEvent(&event);
            }

            const std::shared_ptr<const PikvmVideoFrame> frame =
                take_latest_pikvm_frame(*state, last_sequence);
            if (frame) {
                last_sequence = frame->sequence;
                if (texture == nullptr || texture_width != frame->width || texture_height != frame->height) {
                    if (texture != nullptr) {
                        SDL_DestroyTexture(texture);
                    }
                    texture = SDL_CreateTexture(
                        renderer,
                        SDL_PIXELFORMAT_RGBA32,
                        SDL_TEXTUREACCESS_STREAMING,
                        frame->width,
                        frame->height);
                    if (texture == nullptr) {
                        throw_sdl_error("SDL_CreateTexture");
                    }
                    texture_width = frame->width;
                    texture_height = frame->height;
                    SDL_SetWindowTitle(
                        window,
                        ("hitsc - PiKVM - " + options.login.base_url.host + " - "
                         + std::to_string(frame->width) + "x" + std::to_string(frame->height))
                            .c_str());
                }

                if (!SDL_UpdateTexture(texture, nullptr, frame->rgba.data(), frame->width * 4)) {
                    throw_sdl_error("SDL_UpdateTexture");
                }

                ++presented_frames;
                if (options.login.verbose && (presented_frames <= 20 || presented_frames % 60 == 0)) {
                    log_info() << "presented PiKVM frame #" << presented_frames
                               << " sequence=" << frame->sequence
                               << " avg-rgb=" << sampled_average_rgb(frame->rgba);
                }
                render_needed = true;
            }

            if (render_needed) {
                SDL_SetRenderDrawColor(renderer, 12, 14, 18, 255);
                SDL_RenderClear(renderer);
                if (texture != nullptr && texture_width > 0 && texture_height > 0) {
                    const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                    SDL_RenderTexture(renderer, texture, nullptr, &target);
                }
                SDL_RenderPresent(renderer);
                first_render = false;
            }

            const std::uint64_t ticks = SDL_GetTicks();
            if (texture == nullptr && ticks - last_status_tick > 1000) {
                last_status_tick = ticks;
                SDL_SetWindowTitle(window, ("hitsc - PiKVM - " + pikvm_status_snapshot(*state)).c_str());
            }

            if (network_done->load()) {
                if (std::exception_ptr exception = take_pikvm_exception(*state)) {
                    std::rethrow_exception(exception);
                }
                running = false;
            }
        }
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "pikvm view ui thread");
        stop_pikvm_network(*state, *stop_requested, network_thread, options.login.verbose);
        if (texture != nullptr) {
            SDL_DestroyTexture(texture);
        }
        if (renderer != nullptr) {
            SDL_DestroyRenderer(renderer);
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
        throw;
    }

    stop_pikvm_network(*state, *stop_requested, network_thread, options.login.verbose);

    if (texture != nullptr) {
        SDL_DestroyTexture(texture);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

} // namespace hitsc
