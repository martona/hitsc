#include "aten_view.hpp"

#include "aspeed_decoder.hpp"
#include "aten_network.hpp"
#include "aten_protocol.hpp"
#include "diagnostics.hpp"
#include "log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace hitsc {

extern std::atomic_bool g_aten_full_framebuffer_refresh_requested;

namespace {

constexpr std::uint64_t kMouseMotionIntervalMilliseconds = 10;
using AtenKeyDownState = std::array<bool, 256>;

struct AtenRemoteMousePosition {
    int x = 0;
    int y = 0;
};

struct AtenPresentationSlot {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
    bool valid = false;
    std::vector<std::uint8_t> shadow;
};

struct LockedTexture {
    SDL_Texture* texture = nullptr;

    ~LockedTexture()
    {
        unlock();
    }

    void unlock()
    {
        if (texture != nullptr) {
            SDL_UnlockTexture(texture);
            texture = nullptr;
        }
    }
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

void destroy_slot(AtenPresentationSlot& slot)
{
    if (slot.texture != nullptr) {
        SDL_DestroyTexture(slot.texture);
        slot.texture = nullptr;
    }
    slot.width = 0;
    slot.height = 0;
    slot.valid = false;
    slot.shadow.clear();
}

void destroy_slots(std::array<AtenPresentationSlot, 2>& slots)
{
    for (AtenPresentationSlot& slot : slots) {
        destroy_slot(slot);
    }
}

void ensure_slot_texture(SDL_Renderer* renderer, AtenPresentationSlot& slot, int width, int height)
{
    if (slot.texture != nullptr && slot.width == width && slot.height == height) {
        return;
    }

    destroy_slot(slot);
    slot.texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING,
        width,
        height);
    if (slot.texture == nullptr) {
        throw_sdl_error("SDL_CreateTexture");
    }
    slot.width = width;
    slot.height = height;
}

std::size_t frame_rgba_size(int width, int height)
{
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
}

void seed_framebuffer(
    std::uint8_t* target,
    std::size_t size,
    const AtenPresentationSlot* previous_slot)
{
    if (previous_slot != nullptr && previous_slot->valid && previous_slot->shadow.size() == size) {
        std::memcpy(target, previous_slot->shadow.data(), size);
        return;
    }
    std::fill(target, target + size, 0xff);
}

bool try_decode_direct_to_texture(
    AspeedDecoder& decoder,
    const AtenCompressedFrame& frame,
    AtenPresentationSlot& target_slot,
    const AtenPresentationSlot* previous_slot)
{
    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(target_slot.texture, nullptr, &pixels, &pitch)) {
        return false;
    }

    LockedTexture locked{target_slot.texture};
    const int expected_pitch = frame.width * 4;
    if (pitch != expected_pitch) {
        return false;
    }

    const std::size_t size = frame_rgba_size(frame.width, frame.height);
    auto* rgba = static_cast<std::uint8_t*>(pixels);
    seed_framebuffer(rgba, size, previous_slot);
    decoder.decode_rgba_into(frame.decode_options, frame.compressed, std::span<std::uint8_t>(rgba, size));

    target_slot.shadow.resize(size);
    std::memcpy(target_slot.shadow.data(), rgba, size);
    target_slot.valid = true;
    locked.unlock();
    return true;
}

void decode_to_shadow_and_upload(
    AspeedDecoder& decoder,
    const AtenCompressedFrame& frame,
    AtenPresentationSlot& target_slot,
    const AtenPresentationSlot* previous_slot)
{
    const std::size_t size = frame_rgba_size(frame.width, frame.height);
    target_slot.shadow.resize(size);
    seed_framebuffer(target_slot.shadow.data(), size, previous_slot);
    decoder.decode_rgba_into(
        frame.decode_options,
        frame.compressed,
        std::span<std::uint8_t>(target_slot.shadow.data(), target_slot.shadow.size()));

    if (!SDL_UpdateTexture(target_slot.texture, nullptr, target_slot.shadow.data(), frame.width * 4)) {
        throw_sdl_error("SDL_UpdateTexture");
    }
    target_slot.valid = true;
}

bool present_aten_frame_to_backbuffer(
    SDL_Renderer* renderer,
    AspeedDecoder& decoder,
    std::array<AtenPresentationSlot, 2>& slots,
    int& active_slot,
    const AtenCompressedFrame& frame)
{
    if (active_slot >= 0 &&
        (slots[active_slot].width != frame.width || slots[active_slot].height != frame.height)) {
        destroy_slots(slots);
        active_slot = -1;
    }

    const int next_slot = active_slot == 0 ? 1 : 0;
    ensure_slot_texture(renderer, slots[next_slot], frame.width, frame.height);

    const AtenPresentationSlot* previous_slot = active_slot >= 0 ? &slots[active_slot] : nullptr;
    bool direct = false;
    try {
        direct = try_decode_direct_to_texture(decoder, frame, slots[next_slot], previous_slot);
    } catch (...) {
        slots[next_slot].valid = false;
        throw;
    }

    if (!direct) {
        decode_to_shadow_and_upload(decoder, frame, slots[next_slot], previous_slot);
    }

    active_slot = next_slot;
    return direct;
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
    std::array<AtenPresentationSlot, 2> slots;
    int active_slot = -1;

    auto cleanup = [&] {
        destroy_slots(slots);
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
        std::uint64_t last_status_tick = 0;
        int presented_frames = 0;
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
                    destroy_slots(slots);
                    active_slot = -1;
                    last_sequence = 0;
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
                } else if (active_slot >= 0 &&
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

                        const AtenPresentationSlot& active = slots[active_slot];
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
                } else if (active_slot >= 0 && event.type == SDL_EVENT_MOUSE_MOTION) {
                    const std::uint64_t ticks = SDL_GetTicks();
                    const bool throttled =
                        mouse_buttons == 0 &&
                        ticks - last_mouse_motion_ticks < kMouseMotionIntervalMilliseconds;
                    if (!throttled) {
                        const AtenPresentationSlot& active = slots[active_slot];
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
                                mouse_buttons == 0,
                                options.login.verbose);
                            last_mouse_motion_ticks = ticks;
                        }
                    }
                } else if (active_slot >= 0 && event.type == SDL_EVENT_MOUSE_WHEEL) {
                    const AtenPresentationSlot& active = slots[active_slot];
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
                const std::shared_ptr<const AtenCompressedFrame> frame =
                    take_latest_aten_frame(*state, last_sequence);
                if (frame) {
                    last_sequence = frame->sequence;
                    const bool direct = present_aten_frame_to_backbuffer(
                        renderer,
                        decoder,
                        slots,
                        active_slot,
                        *frame);
                    ++presented_frames;
                    if (options.login.verbose && (presented_frames <= 20 || presented_frames % 60 == 0)) {
                        log_info() << "presented ATEN frame #" << presented_frames
                                   << " sequence=" << frame->sequence
                                   << " size=" << frame->width << 'x' << frame->height
                                   << " direct-texture=" << (direct ? "yes" : "no")
                                   << " avg-rgb=" << sampled_average_rgb(slots[active_slot].shadow);
                    }
                    render_needed = true;
                    presented_new_frame = true;
                }

                if (render_needed) {
                    SDL_SetRenderDrawColor(renderer, 12, 14, 18, 255);
                    SDL_RenderClear(renderer);
                    if (active_slot >= 0 && slots[active_slot].texture != nullptr) {
                        const AtenPresentationSlot& active = slots[active_slot];
                        const SDL_FRect target = current_target_rect(window, active.width, active.height);
                        SDL_RenderTexture(renderer, active.texture, nullptr, &target);
                    }
                    SDL_RenderPresent(renderer);
                    if (presented_new_frame && active_slot >= 0) {
                        state->view_status.frame_presented(
                            slots[active_slot].width,
                            slots[active_slot].height);
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

    stop_aten_network(*state, *stop_requested, network_thread, options.login.verbose);
    cleanup();
}

} // namespace hitsc
