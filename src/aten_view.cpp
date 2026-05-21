#include "aten_view.hpp"

#include "aspeed_decoder.hpp"
#include "aspeed_presenter.hpp"
#include "aten_network.hpp"
#include "aten_protocol.hpp"
#include "diagnostics.hpp"
#include "log.hpp"
#include "hardware_cursor.hpp"
#include "hardware_cursor_presenter.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace hitsc {

extern std::atomic_bool g_aten_full_framebuffer_refresh_requested;

namespace {

constexpr std::uint64_t kMouseMotionIntervalMilliseconds = 10;
using AtenKeyDownState = std::array<bool, 256>;

struct AtenRemoteMousePosition {
    int x = 0;
    int y = 0;
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
        throw_sdl_error("SDL_GetWindowSizeInPixels");
    }
    return centered_target_rect(window_width, window_height, frame_width, frame_height);
}

std::optional<AtenRemoteMousePosition> remote_mouse_position(
    float window_x,
    float window_y,
    const SDL_FRect& target,
    int frame_width,
    int frame_height)
{
    if (target.w <= 0.0f || target.h <= 0.0f) {
        return std::nullopt;
    }

    const float relative_x = (window_x - target.x) / target.w;
    const float relative_y = (window_y - target.y) / target.h;
    if (relative_x < 0.0f || relative_x > 1.0f ||
        relative_y < 0.0f || relative_y > 1.0f) {
        return std::nullopt;
    }

    return AtenRemoteMousePosition{
        std::clamp(static_cast<int>(std::lround(relative_x * static_cast<float>(frame_width))), 0, frame_width),
        std::clamp(static_cast<int>(std::lround(relative_y * static_cast<float>(frame_height))), 0, frame_height)};
}

std::uint8_t button_mask_for_sdl_button(std::uint8_t button)
{
    switch (button) {
    case SDL_BUTTON_LEFT:
        return 1;
    case SDL_BUTTON_MIDDLE:
        return 2;
    case SDL_BUTTON_RIGHT:
        return 4;
    default:
        return 0;
    }
}

std::optional<std::uint32_t> aten_keyboard_usage_from_sdl_scancode(SDL_Scancode scancode)
{
    const auto usage = static_cast<int>(scancode);
    if ((usage >= SDL_SCANCODE_A && usage <= SDL_SCANCODE_APPLICATION) ||
        (usage >= SDL_SCANCODE_KP_EQUALS && usage <= SDL_SCANCODE_RGUI)) {
        return static_cast<std::uint32_t>(usage);
    }

    return std::nullopt;
}

void release_all_aten_keys(AtenViewState& state, AtenKeyDownState& key_down, bool verbose)
{
    for (std::size_t usage = 0; usage < key_down.size(); ++usage) {
        if (!key_down[usage]) {
            continue;
        }
        key_down[usage] = false;
        queue_aten_key_event(state, static_cast<std::uint32_t>(usage), false, verbose);
    }
}

} // namespace

void run_aten_view(const AtenViewOptions& options)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw_sdl_error("SDL_Init");
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    auto state = std::make_shared<AtenViewState>();
    auto stop_requested = std::make_shared<std::atomic_bool>(false);
    auto network_done = std::make_shared<std::atomic_bool>(false);
    std::thread network_thread;
    AspeedPresenter presenter;
    HardwareCursorPresenter cursor_presenter;

    auto cleanup = [&] {
        presenter.destroy();
        cursor_presenter.destroy();
        if (renderer != nullptr) {
            SDL_DestroyRenderer(renderer);
            renderer = nullptr;
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        SDL_Quit();
    };

    auto hide_window_for_teardown = [&] {
        if (window != nullptr) {
            SDL_HideWindow(window);
        }
    };

    try {
        const Uint32 frame_event_type = SDL_RegisterEvents(1);
        if (frame_event_type == 0) {
            throw_sdl_error("SDL_RegisterEvents");
        }
        state->frame_event_type.store(frame_event_type);

        window = SDL_CreateWindow("hitsc - ATEN", 1024, 768, SDL_WINDOW_RESIZABLE);
        if (window == nullptr) {
            throw_sdl_error("SDL_CreateWindow");
        }
        SDL_SetWindowTitle(window, state->view_status.title(options.login.base_url.host).c_str());

        renderer = SDL_CreateRenderer(window, nullptr);
        if (renderer == nullptr) {
            throw_sdl_error("SDL_CreateRenderer");
        }

        AtenViewOptions network_options = options;
        network_thread = std::thread([network_options, state, stop_requested, network_done] {
            try {
                run_aten_network_session(network_options, *state, *stop_requested);
            } catch (...) {
                set_aten_exception(*state, std::current_exception());
            }
            {
                std::lock_guard lock(state->control_mutex);
                state->input_sink = {};
                state->pending_input.clear();
                state->force_close = {};
            }
            network_done->store(true);
        });

        AspeedDecoder decoder;
        bool running = true;
        bool first_render = true;
        bool visible = true;
        bool close_event_logged = false;
        std::uint64_t last_sequence = 0;
        std::uint64_t last_cursor_sequence = 0;
        std::uint64_t last_status_tick = 0;
        int presented_frames = 0;
        HardwareCursor hardware_cursor;
        bool has_hardware_cursor = false;
        std::uint8_t mouse_buttons = 0;
        std::uint64_t last_mouse_motion_ticks = 0;
        AtenKeyDownState key_down{};

        while (running) {
            bool render_needed = first_render;
            bool presented_new_frame = false;
            SDL_Event event{};
            bool have_event = SDL_WaitEventTimeout(&event, 16);
            while (have_event) {
                if (event.type == SDL_EVENT_QUIT ||
                    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    if (!close_event_logged) {
                        close_event_logged = true;
                        log_info() << "aten window close event"
                                   << " type=" << event.type;
                    }
                    running = false;
                } else if (event.type == SDL_EVENT_WINDOW_MINIMIZED ||
                           event.type == SDL_EVENT_WINDOW_HIDDEN) {
                    visible = false;
                    state->frame_event_pending.store(false);
                    state->view_status.minimize();
                    clear_latest_aten_frame(*state);
                    presenter.destroy();
                    cursor_presenter.destroy();
                    last_sequence = 0;
                    last_cursor_sequence = 0;
                    last_status_tick = 0;
                    SDL_SetWindowTitle(window, state->view_status.title(options.login.base_url.host).c_str());
                } else if (event.type == SDL_EVENT_WINDOW_RESTORED ||
                           event.type == SDL_EVENT_WINDOW_SHOWN) {
                    visible = true;
                    g_aten_full_framebuffer_refresh_requested.store(true);
                    render_needed = true;
                    last_status_tick = 0;
                    SDL_SetWindowTitle(window, state->view_status.title(options.login.base_url.host).c_str());
                } else if (event.type == state->frame_event_type.load()) {
                    state->frame_event_pending.store(false);
                    render_needed = true;
                } else if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                           event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                           event.type == SDL_EVENT_WINDOW_EXPOSED) {
                    render_needed = true;
                } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    release_all_aten_keys(*state, key_down, options.login.verbose);
                } else if (event.type == SDL_EVENT_KEY_DOWN ||
                           event.type == SDL_EVENT_KEY_UP) {
                    if (!(event.type == SDL_EVENT_KEY_DOWN && event.key.repeat)) {
                        const std::optional<std::uint32_t> usage =
                            aten_keyboard_usage_from_sdl_scancode(event.key.scancode);
                        if (!usage || *usage >= key_down.size()) {
                            if (options.login.verbose) {
                                log_info() << "ignored ATEN key"
                                           << " scancode=" << event.key.scancode
                                           << " key=" << event.key.key;
                            }
                        } else {
                            const bool down = event.type == SDL_EVENT_KEY_DOWN;
                            if (key_down[*usage] != down) {
                                key_down[*usage] = down;
                                queue_aten_key_event(*state, *usage, down, options.login.verbose);
                            }
                        }
                    }
                } else if (presenter.active_slot() != nullptr &&
                           (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                            event.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
                    const std::uint8_t mask = button_mask_for_sdl_button(event.button.button);
                    if (mask != 0) {
                        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                            mouse_buttons |= mask;
                        } else {
                            mouse_buttons &= static_cast<std::uint8_t>(~mask);
                        }
                        SDL_CaptureMouse(mouse_buttons != 0);

                        const AspeedPresentationSlot& active = *presenter.active_slot();
                        const SDL_FRect target = current_target_rect(window, active.width, active.height);
                        const std::optional<AtenRemoteMousePosition> position = remote_mouse_position(
                            event.button.x,
                            event.button.y,
                            target,
                            active.width,
                            active.height);
                        if (position) {
                            queue_aten_pointer_event(
                                *state,
                                position->x,
                                position->y,
                                mouse_buttons,
                                false,
                                options.login.verbose);
                        }
                    }
                } else if (presenter.active_slot() != nullptr && event.type == SDL_EVENT_MOUSE_MOTION) {
                    const std::uint64_t ticks = SDL_GetTicks();
                    const bool throttled =
                        mouse_buttons == 0 &&
                        ticks - last_mouse_motion_ticks < kMouseMotionIntervalMilliseconds;
                    if (!throttled) {
                        const AspeedPresentationSlot& active = *presenter.active_slot();
                        const SDL_FRect target = current_target_rect(window, active.width, active.height);
                        const std::optional<AtenRemoteMousePosition> position = remote_mouse_position(
                            event.motion.x,
                            event.motion.y,
                            target,
                            active.width,
                            active.height);
                        if (position) {
                            queue_aten_pointer_event(
                                *state,
                                position->x,
                                position->y,
                                mouse_buttons,
                                !options.login.debug_disable_input_coalescing && mouse_buttons == 0,
                                options.login.verbose);
                            last_mouse_motion_ticks = ticks;
                        }
                    }
                } else if (presenter.active_slot() != nullptr && event.type == SDL_EVENT_MOUSE_WHEEL) {
                    const AspeedPresentationSlot& active = *presenter.active_slot();
                    const SDL_FRect target = current_target_rect(window, active.width, active.height);
                    const std::optional<AtenRemoteMousePosition> position = remote_mouse_position(
                        event.wheel.mouse_x,
                        event.wheel.mouse_y,
                        target,
                        active.width,
                        active.height);
                    if (position) {
                        const float y = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
                            ? -event.wheel.y
                            : event.wheel.y;
                        if (y != 0.0f) {
                            const std::uint8_t wheel_mask = y > 0.0f ? 8U : 16U;
                            queue_aten_pointer_event(
                                *state,
                                position->x,
                                position->y,
                                wheel_mask,
                                false,
                                options.login.verbose);
                            queue_aten_pointer_event(
                                *state,
                                position->x,
                                position->y,
                                0,
                                false,
                                options.login.verbose);
                        }
                    }
                }
                have_event = SDL_PollEvent(&event);
            }

            if (visible) {
                bool cursor_texture_dirty = false;
                if (std::optional<HardwareCursor> cursor =
                        take_latest_aten_cursor(*state, last_cursor_sequence)) {
                    hardware_cursor = std::move(*cursor);
                    last_cursor_sequence = hardware_cursor.sequence;
                    has_hardware_cursor = true;
                    cursor_texture_dirty = true;
                }

                const std::shared_ptr<const AtenCompressedFrame> frame =
                    take_latest_aten_frame(*state, last_sequence);
                if (frame) {
                    last_sequence = frame->sequence;
                    const bool direct = presenter.present(
                        renderer,
                        decoder,
                        frame->width,
                        frame->height,
                        frame->decode_options,
                        frame->compressed);
                    const AspeedPresentationSlot* active = presenter.active_slot();
                    ++presented_frames;
                    if (active != nullptr &&
                        options.login.verbose && (presented_frames <= 20 || presented_frames % 60 == 0)) {
                        log_info() << "presented ATEN frame #" << presented_frames
                                   << " sequence=" << frame->sequence
                                   << " size=" << frame->width << 'x' << frame->height
                                   << " direct-texture=" << (direct ? "yes" : "no")
                                   << " avg-rgb=" << aspeed_sampled_average_rgb(active->shadow);
                    }
                    render_needed = true;
                    presented_new_frame = true;
                    cursor_texture_dirty = has_hardware_cursor;
                }

                if (cursor_texture_dirty && has_hardware_cursor) {
                    const AspeedPresentationSlot* active = presenter.active_slot();
                    if (active != nullptr) {
                        const CursorImage cursor_image = make_cursor_image(
                            hardware_cursor,
                            active->shadow,
                            active->width,
                            active->height);
                        cursor_presenter.update(renderer, cursor_image);
                        render_needed = true;
                    }
                }

                if (render_needed) {
                    SDL_SetRenderDrawColor(renderer, 12, 14, 18, 255);
                    SDL_RenderClear(renderer);
                    if (const AspeedPresentationSlot* active = presenter.active_slot();
                        active != nullptr && active->texture != nullptr) {
                        const SDL_FRect target = current_target_rect(window, active->width, active->height);
                        SDL_RenderTexture(renderer, active->texture, nullptr, &target);
                        if (has_hardware_cursor) {
                            cursor_presenter.render(
                                renderer,
                                hardware_cursor,
                                target,
                                active->width,
                                active->height);
                        }
                    }
                    SDL_RenderPresent(renderer);
                    if (presented_new_frame) {
                        const AspeedPresentationSlot* active = presenter.active_slot();
                        if (active != nullptr) {
                            state->view_status.frame_presented(
                                active->width,
                                active->height);
                        }
                    }
                    first_render = false;
                }
            }

            const std::uint64_t ticks = SDL_GetTicks();
            if (ticks - last_status_tick >= 1000) {
                last_status_tick = ticks;
                SDL_SetWindowTitle(window, state->view_status.title(options.login.base_url.host).c_str());
            }

            if (network_done->load()) {
                if (std::exception_ptr exception = take_aten_exception(*state)) {
                    std::rethrow_exception(exception);
                }
                running = false;
            }
        }
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "aten view ui thread");
        stop_aten_network(*state, *stop_requested, network_thread, options.login.verbose);
        cleanup();
        throw;
    }

    hide_window_for_teardown();
    stop_aten_network(*state, *stop_requested, network_thread, options.login.verbose);
    cleanup();
}

} // namespace hitsc
