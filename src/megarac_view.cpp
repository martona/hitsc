#include "megarac_view.hpp"

#include "aspeed_decoder.hpp"
#include "aspeed_presenter.hpp"
#include "diagnostics.hpp"
#include "log.hpp"
#include "hardware_cursor.hpp"
#include "hardware_cursor_presenter.hpp"
#include "megarac_hid.hpp"
#include "megarac_protocol.hpp"
#include "megarac_view_session.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hitsc {
namespace {

constexpr std::uint64_t kMouseMotionIntervalMilliseconds = 8;

constexpr std::uint16_t kCmdSendHidPacket = command_value(MegaracCommand::SendHidPacket);
constexpr std::uint16_t kCmdGetFullScreen = command_value(MegaracCommand::GetFullScreen);
using KeyboardKeySlots = MegaracKeyboardKeySlots;
using SharedCursor = MegaracHardwareCursor;

constexpr int kRelativeMouseMode = kMegaracRelativeMouseMode;
constexpr int kOtherMouseMode = kMegaracOtherMouseMode;
constexpr std::uint8_t kKeyboardLeftCtrl = kMegaracKeyboardLeftCtrl;
constexpr std::uint8_t kKeyboardLeftShift = kMegaracKeyboardLeftShift;
constexpr std::uint8_t kKeyboardLeftAlt = kMegaracKeyboardLeftAlt;
constexpr std::uint8_t kKeyboardLeftGui = kMegaracKeyboardLeftGui;
constexpr std::uint8_t kKeyboardRightCtrl = kMegaracKeyboardRightCtrl;
constexpr std::uint8_t kKeyboardRightShift = kMegaracKeyboardRightShift;
constexpr std::uint8_t kKeyboardRightAlt = kMegaracKeyboardRightAlt;
constexpr std::uint8_t kKeyboardRightGui = kMegaracKeyboardRightGui;
constexpr std::uint8_t kMouseLeftButton = kMegaracMouseLeftButton;
constexpr std::uint8_t kMouseRightButton = kMegaracMouseRightButton;
constexpr std::uint8_t kMouseMiddleButton = kMegaracMouseMiddleButton;

struct RemoteMousePosition {
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
        SDL_GetWindowSize(window, &window_width, &window_height);
    }
    return centered_target_rect(window_width, window_height, frame_width, frame_height);
}

std::optional<RemoteMousePosition> remote_mouse_position(
    float window_x,
    float window_y,
    const SDL_FRect& target,
    int frame_width,
    int frame_height)
{
    if (frame_width <= 0 || frame_height <= 0 || target.w <= 0.0f || target.h <= 0.0f) {
        return std::nullopt;
    }
    if (window_x < target.x || window_y < target.y ||
        window_x > target.x + target.w || window_y > target.y + target.h) {
        return std::nullopt;
    }

    const double normalized_x =
        (static_cast<double>(window_x) - static_cast<double>(target.x)) / static_cast<double>(target.w);
    const double normalized_y =
        (static_cast<double>(window_y) - static_cast<double>(target.y)) / static_cast<double>(target.h);
    return RemoteMousePosition{
        std::clamp(static_cast<int>(std::floor(normalized_x * frame_width + 0.5)), 0, frame_width),
        std::clamp(static_cast<int>(std::floor(normalized_y * frame_height + 0.5)), 0, frame_height),
    };
}

std::uint8_t button_mask_for_sdl_button(std::uint8_t button)
{
    switch (button) {
    case SDL_BUTTON_LEFT:
        return kMouseLeftButton;
    case SDL_BUTTON_RIGHT:
        return kMouseRightButton;
    case SDL_BUTTON_MIDDLE:
        return kMouseMiddleButton;
    default:
        return 0;
    }
}

std::optional<std::uint8_t> keyboard_modifier_bit(SDL_Scancode scancode)
{
    switch (scancode) {
    case SDL_SCANCODE_LCTRL:
        return kKeyboardLeftCtrl;
    case SDL_SCANCODE_LSHIFT:
        return kKeyboardLeftShift;
    case SDL_SCANCODE_LALT:
        return kKeyboardLeftAlt;
    case SDL_SCANCODE_LGUI:
        return kKeyboardLeftGui;
    case SDL_SCANCODE_RCTRL:
        return kKeyboardRightCtrl;
    case SDL_SCANCODE_RSHIFT:
        return kKeyboardRightShift;
    case SDL_SCANCODE_RALT:
        return kKeyboardRightAlt;
    case SDL_SCANCODE_RGUI:
        return kKeyboardRightGui;
    default:
        return std::nullopt;
    }
}

std::optional<std::uint8_t> keyboard_usage_from_sdl_scancode(SDL_Scancode scancode)
{
    const auto usage = static_cast<int>(scancode);
    if ((usage >= SDL_SCANCODE_A && usage <= SDL_SCANCODE_APPLICATION) ||
        (usage >= SDL_SCANCODE_KP_EQUALS && usage <= SDL_SCANCODE_F24)) {
        return static_cast<std::uint8_t>(usage);
    }

    return std::nullopt;
}

bool set_keyboard_usage(KeyboardKeySlots& keys, std::uint8_t usage, bool pressed)
{
    const auto existing = std::find(keys.begin(), keys.end(), usage);
    if (pressed) {
        if (existing != keys.end()) {
            return false;
        }

        const auto empty = std::find(keys.begin(), keys.end(), 0);
        if (empty == keys.end()) {
            return false;
        }

        *empty = usage;
        return true;
    }

    if (existing == keys.end()) {
        return false;
    }

    *existing = 0;
    return true;
}

bool has_keyboard_state(std::uint8_t modifiers, const KeyboardKeySlots& keys)
{
    return modifiers != 0 || std::any_of(keys.begin(), keys.end(), [](std::uint8_t key) {
        return key != 0;
    });
}

void send_keyboard_report(
    MegaracViewSessionState& state,
    std::uint8_t modifiers,
    const KeyboardKeySlots& keys,
    std::uint32_t& sequence,
    bool verbose)
{
    std::vector<std::uint8_t> packet =
        make_megarac_keyboard_packet(MegaracKeyboardReport{modifiers, keys}, sequence++);
    const bool accepted = queue_megarac_view_packet(state, kCmdSendHidPacket, std::move(packet));
    if (verbose && accepted) {
        LogLine line = log_info();
        line << "queued keyboard"
             << " modifiers=0x" << std::hex << std::setw(2) << std::setfill('0')
             << static_cast<int>(modifiers)
             << std::dec << std::setfill(' ')
             << " keys=";
        bool first = true;
        for (const std::uint8_t key : keys) {
            if (key == 0) {
                continue;
            }
            if (!first) {
                line << ',';
            }
            first = false;
            line << static_cast<int>(key);
        }
        if (first) {
            line << "none";
        }
    }
}

void send_mouse_report(
    MegaracViewSessionState& state,
    std::uint8_t buttons,
    const RemoteMousePosition& position,
    int frame_width,
    int frame_height,
    int wheel,
    std::optional<RemoteMousePosition>& last_relative_position,
    std::uint32_t& sequence,
    bool verbose)
{
    const int mouse_mode = megarac_view_mouse_mode_snapshot(state);
    std::vector<std::uint8_t> packet;
    if (mouse_mode == kRelativeMouseMode || mouse_mode == kOtherMouseMode) {
        const int dx = last_relative_position ? position.x - last_relative_position->x : 0;
        const int dy = last_relative_position ? position.y - last_relative_position->y : 0;
        packet = make_megarac_relative_mouse_packet(
            MegaracRelativeMouseReport{buttons, dx, dy, wheel},
            sequence++);
    } else {
        packet = make_megarac_absolute_mouse_packet(
            MegaracAbsoluteMouseReport{buttons, position.x, position.y, frame_width, frame_height, wheel},
            sequence++);
    }

    last_relative_position = position;
    const bool accepted = queue_megarac_view_packet(state, kCmdSendHidPacket, std::move(packet));
    if (verbose && accepted) {
        log_info() << "queued mouse"
                   << " mode=" << mouse_mode
                   << " buttons=" << static_cast<int>(buttons)
                   << " x=" << position.x
                   << " y=" << position.y
                   << " wheel=" << wheel;
    }
}

} // namespace

void run_megarac_view(const MegaracViewOptions& options)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw_sdl_error("SDL_Init");
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;

    auto stop_requested = std::make_shared<std::atomic_bool>(false);
    auto network_done = std::make_shared<std::atomic_bool>(false);
    auto state = std::make_shared<MegaracViewSessionState>();
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

        window = SDL_CreateWindow("hitsc", 1024, 768, SDL_WINDOW_RESIZABLE);
        if (window == nullptr) {
            throw_sdl_error("SDL_CreateWindow");
        }
        SDL_SetWindowTitle(window, state->view_status.title(options.login.base_url.host).c_str());

        renderer = SDL_CreateRenderer(window, nullptr);
        if (renderer == nullptr) {
            throw_sdl_error("SDL_CreateRenderer");
        }

        MegaracViewOptions network_options = options;
        network_thread = std::thread([network_options, state, stop_requested, network_done] {
            try {
                run_megarac_view_session(network_options, *state, *stop_requested);
            } catch (...) {
                set_megarac_exception(*state, std::current_exception());
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
        SharedCursor hardware_cursor;
        bool has_hardware_cursor = false;
        std::uint64_t last_cursor_sequence = 0;
        std::uint8_t mouse_buttons = 0;
        std::uint32_t mouse_sequence = 0;
        std::uint8_t keyboard_modifiers = 0;
        KeyboardKeySlots keyboard_keys{};
        std::uint32_t keyboard_sequence = 0;
        std::uint64_t last_mouse_motion_ticks = 0;
        std::optional<RemoteMousePosition> last_relative_mouse_position;

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
                        log_info() << "megarac window close event"
                                   << " type=" << event.type;
                    }
                    running = false;
                } else if (event.type == SDL_EVENT_WINDOW_MINIMIZED ||
                           event.type == SDL_EVENT_WINDOW_HIDDEN) {
                    visible = false;
                    state->frame_event_pending.store(false);
                    state->view_status.minimize();
                    clear_latest_megarac_view_frame(*state);
                    presenter.destroy();
                    cursor_presenter.destroy();
                    last_sequence = 0;
                    last_status_tick = 0;
                    SDL_SetWindowTitle(window, state->view_status.title(options.login.base_url.host).c_str());
                } else if (event.type == SDL_EVENT_WINDOW_RESTORED ||
                           event.type == SDL_EVENT_WINDOW_SHOWN) {
                    visible = true;
                    clear_latest_megarac_view_frame(*state);
                    last_sequence = 0;
                    queue_megarac_view_packet(
                        *state,
                        kCmdGetFullScreen,
                        make_simple_packet(kCmdGetFullScreen, 1));
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
                    if (has_keyboard_state(keyboard_modifiers, keyboard_keys)) {
                        keyboard_modifiers = 0;
                        keyboard_keys.fill(0);
                        send_keyboard_report(
                            *state,
                            keyboard_modifiers,
                            keyboard_keys,
                            keyboard_sequence,
                            options.login.vverbose);
                    }
                } else if (event.type == SDL_EVENT_KEY_DOWN ||
                           event.type == SDL_EVENT_KEY_UP) {
                    if (options.login.vverbose) {
                        log_info() << "key "
                                   << (event.type == SDL_EVENT_KEY_DOWN ? "down" : "up")
                                   << " scancode=" << event.key.scancode
                                   << " key=" << event.key.key;
                    }

                    if (!(event.type == SDL_EVENT_KEY_DOWN && event.key.repeat)) {
                        const bool pressed = event.type == SDL_EVENT_KEY_DOWN;
                        const std::optional<std::uint8_t> modifier = keyboard_modifier_bit(event.key.scancode);
                        bool changed = false;
                        if (modifier) {
                            if (pressed) {
                                changed = (keyboard_modifiers & *modifier) == 0;
                                keyboard_modifiers |= *modifier;
                            } else {
                                changed = (keyboard_modifiers & *modifier) != 0;
                                keyboard_modifiers &= static_cast<std::uint8_t>(~*modifier);
                            }
                        } else if (const std::optional<std::uint8_t> usage =
                                       keyboard_usage_from_sdl_scancode(event.key.scancode)) {
                            changed = set_keyboard_usage(keyboard_keys, *usage, pressed);
                        }

                        if (changed) {
                            send_keyboard_report(
                                *state,
                                keyboard_modifiers,
                                keyboard_keys,
                                keyboard_sequence,
                                options.login.vverbose);
                        }
                    }
                } else if (presenter.active_slot() != nullptr &&
                           (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                            event.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
                    const AspeedPresentationSlot& active = *presenter.active_slot();
                    const std::uint8_t mask = button_mask_for_sdl_button(event.button.button);
                    if (mask != 0) {
                        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                            mouse_buttons |= mask;
                        } else {
                            mouse_buttons &= static_cast<std::uint8_t>(~mask);
                        }
                        SDL_CaptureMouse(mouse_buttons != 0);

                        const SDL_FRect target = current_target_rect(window, active.width, active.height);
                        const std::optional<RemoteMousePosition> position = remote_mouse_position(
                            event.button.x,
                            event.button.y,
                            target,
                            active.width,
                            active.height);
                        if (position) {
                            send_mouse_report(
                                *state,
                                mouse_buttons,
                                *position,
                                active.width,
                                active.height,
                                0,
                                last_relative_mouse_position,
                                mouse_sequence,
                                options.login.vverbose);
                        }
                    }
                } else if (presenter.active_slot() != nullptr && event.type == SDL_EVENT_MOUSE_MOTION) {
                    const AspeedPresentationSlot& active = *presenter.active_slot();
                    const std::uint64_t ticks = SDL_GetTicks();
                    const bool throttled =
                        mouse_buttons == 0 &&
                        ticks - last_mouse_motion_ticks < kMouseMotionIntervalMilliseconds;
                    if (!throttled) {
                        const SDL_FRect target = current_target_rect(window, active.width, active.height);
                        const std::optional<RemoteMousePosition> position = remote_mouse_position(
                            event.motion.x,
                            event.motion.y,
                            target,
                            active.width,
                            active.height);
                        if (position) {
                            send_mouse_report(
                                *state,
                                mouse_buttons,
                                *position,
                                active.width,
                                active.height,
                                0,
                                last_relative_mouse_position,
                                mouse_sequence,
                                options.login.vverbose);
                            last_mouse_motion_ticks = ticks;
                        }
                    }
                } else if (presenter.active_slot() != nullptr && event.type == SDL_EVENT_MOUSE_WHEEL) {
                    const AspeedPresentationSlot& active = *presenter.active_slot();
                    const SDL_FRect target = current_target_rect(window, active.width, active.height);
                    const std::optional<RemoteMousePosition> position = remote_mouse_position(
                        event.wheel.mouse_x,
                        event.wheel.mouse_y,
                        target,
                        active.width,
                        active.height);
                    if (position) {
                        const int wheel = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
                            ? static_cast<int>(-event.wheel.y)
                            : static_cast<int>(event.wheel.y);
                        send_mouse_report(
                            *state,
                            mouse_buttons,
                            *position,
                            active.width,
                            active.height,
                            wheel,
                            last_relative_mouse_position,
                            mouse_sequence,
                            options.login.vverbose);
                    }
                }
                have_event = SDL_PollEvent(&event);
            }

            bool cursor_texture_dirty = false;
            if (std::optional<SharedCursor> cursor = take_latest_megarac_view_cursor(*state, last_cursor_sequence)) {
                hardware_cursor = std::move(*cursor);
                last_cursor_sequence = hardware_cursor.sequence;
                has_hardware_cursor = true;
                cursor_texture_dirty = true;
            }

            if (visible) {
                const std::shared_ptr<const MegaracCompressedFrame> frame =
                    take_latest_megarac_view_frame(*state, last_sequence);
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
                    cursor_texture_dirty = has_hardware_cursor;

                    ++presented_frames;
                    if (active != nullptr &&
                        options.login.vverbose && (presented_frames <= 20 || presented_frames % 60 == 0)) {
                        log_info() << "presented MegaRAC frame #" << presented_frames
                                   << " sequence=" << frame->sequence
                                   << " size=" << frame->width << 'x' << frame->height
                                   << " direct-texture=" << (direct ? "yes" : "no")
                                   << " avg-rgb=" << aspeed_sampled_average_rgb(active->shadow);
                    }
                    render_needed = true;
                    presented_new_frame = true;
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
                            state->view_status.frame_presented(active->width, active->height);
                            state->frames_presented.fetch_add(1);
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
                if (std::exception_ptr exception = take_megarac_exception(*state)) {
                    std::rethrow_exception(exception);
                }
                running = false;
            }
        }
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "megarac view ui thread");
        stop_megarac_network(*state, *stop_requested, network_thread, options.login.verbose);
        cleanup();
        throw;
    }

    hide_window_for_teardown();
    stop_megarac_network(*state, *stop_requested, network_thread, options.login.verbose);
    cleanup();
}

} // namespace hitsc
