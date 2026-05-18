#include "megarac_view.hpp"

#include "diagnostics.hpp"
#include "log.hpp"
#include "megarac_cursor.hpp"
#include "megarac_hid.hpp"
#include "megarac_view_session.hpp"
#include "megarac_protocol.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hitsc {
namespace {

constexpr std::uint64_t kMouseMotionIntervalMilliseconds = 8;
constexpr int kMaxSdlEventsPerFrame = 96;
constexpr int kInitialFramebufferWidth = 800;
constexpr int kInitialFramebufferHeight = 600;

constexpr std::uint16_t kCmdSendHidPacket = command_value(MegaracCommand::SendHidPacket);
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

void update_cursor_texture(
    SDL_Renderer* renderer,
    SDL_Texture*& texture,
    int& texture_width,
    int& texture_height,
    const CursorImage& image)
{
    if (image.rgba.empty() || image.width <= 0 || image.height <= 0) {
        if (texture != nullptr) {
            SDL_DestroyTexture(texture);
            texture = nullptr;
        }
        texture_width = 0;
        texture_height = 0;
        return;
    }

    if (texture == nullptr || texture_width != image.width || texture_height != image.height) {
        if (texture != nullptr) {
            SDL_DestroyTexture(texture);
        }
        texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING,
            image.width,
            image.height);
        if (texture == nullptr) {
            throw_sdl_error("SDL_CreateTexture(cursor)");
        }
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        texture_width = image.width;
        texture_height = image.height;
    }

    if (!SDL_UpdateTexture(texture, nullptr, image.rgba.data(), image.width * 4)) {
        throw_sdl_error("SDL_UpdateTexture(cursor)");
    }
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
    const bool accepted = queue_megarac_view_packet(state, kCmdSendHidPacket, std::move(packet), false);
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
    const bool coalesce = buttons == 0 && wheel == 0;
    const bool accepted = queue_megarac_view_packet(state, kCmdSendHidPacket, std::move(packet), coalesce);
    if (verbose && accepted) {
        log_info() << "queued mouse"
                   << " mode=" << mouse_mode
                   << " buttons=" << static_cast<int>(buttons)
                   << " x=" << position.x
                   << " y=" << position.y
                   << " wheel=" << wheel;
    }
}

void stop_network_thread(
    MegaracViewSessionState& state,
    std::atomic_bool& stop_requested,
    std::thread& network_thread,
    const std::atomic_bool& network_done)
{
    stop_requested.store(true);
    stop_megarac_view_session(state);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    while (!network_done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!network_thread.joinable()) {
        return;
    }
    if (network_done.load()) {
        network_thread.join();
    } else {
        network_thread.detach();
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
    SDL_Texture* texture = nullptr;
    SDL_Texture* cursor_texture = nullptr;

    auto stop_requested = std::make_shared<std::atomic_bool>(false);
    auto network_done = std::make_shared<std::atomic_bool>(false);
    auto state = std::make_shared<MegaracViewSessionState>();
    std::thread network_thread;

    try {
        window = SDL_CreateWindow("hitsc", 1024, 768, SDL_WINDOW_RESIZABLE);
        if (window == nullptr) {
            throw_sdl_error("SDL_CreateWindow");
        }

        renderer = SDL_CreateRenderer(window, nullptr);
        if (renderer == nullptr) {
            throw_sdl_error("SDL_CreateRenderer");
        }

        MegaracViewOptions network_options = options;
        network_thread = std::thread([network_options, state, stop_requested, network_done] {
            run_megarac_view_session(network_options, *state, *stop_requested);
            network_done->store(true);
        });

        bool running = true;
        std::uint64_t last_sequence = 0;
        std::uint64_t last_status_tick = 0;
        int presented_frames = 0;
        int texture_width = kInitialFramebufferWidth;
        int texture_height = kInitialFramebufferHeight;
        int cursor_texture_width = 0;
        int cursor_texture_height = 0;
        SharedCursor hardware_cursor;
        bool has_hardware_cursor = false;
        std::uint64_t last_cursor_sequence = 0;
        std::vector<std::uint8_t> latest_frame_rgba;
        std::uint8_t mouse_buttons = 0;
        std::uint32_t mouse_sequence = 0;
        std::uint8_t keyboard_modifiers = 0;
        KeyboardKeySlots keyboard_keys{};
        std::uint32_t keyboard_sequence = 0;
        std::uint64_t last_mouse_motion_ticks = 0;
        std::optional<RemoteMousePosition> last_relative_mouse_position;

        while (running) {
            if (network_done->load()) {
                running = false;
                continue;
            }

            SDL_Event event{};
            std::optional<RemoteMousePosition> pending_mouse_motion;
            std::uint64_t pending_mouse_motion_ticks = 0;
            int events_processed = 0;
            while (events_processed < kMaxSdlEventsPerFrame && SDL_PollEvent(&event)) {
                ++events_processed;
                if (event.type == SDL_EVENT_QUIT ||
                    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    running = false;
                } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    if (has_keyboard_state(keyboard_modifiers, keyboard_keys)) {
                        keyboard_modifiers = 0;
                        keyboard_keys.fill(0);
                        send_keyboard_report(
                            *state,
                            keyboard_modifiers,
                            keyboard_keys,
                            keyboard_sequence,
                            options.login.verbose);
                    }
                } else if (event.type == SDL_EVENT_KEY_DOWN ||
                           event.type == SDL_EVENT_KEY_UP) {
                    if (options.login.verbose) {
                        log_info() << "key "
                                   << (event.type == SDL_EVENT_KEY_DOWN ? "down" : "up")
                                   << " scancode=" << event.key.scancode
                                   << " key=" << event.key.key;
                    }

                    if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat) {
                        continue;
                    }

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
                            options.login.verbose);
                    }
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                           event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    if (texture_width > 0 && texture_height > 0) {
                        const std::uint8_t mask = button_mask_for_sdl_button(event.button.button);
                        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                            mouse_buttons |= mask;
                        } else {
                            mouse_buttons &= static_cast<std::uint8_t>(~mask);
                        }
                        if (mouse_buttons != 0) {
                            SDL_CaptureMouse(true);
                        } else {
                            SDL_CaptureMouse(false);
                        }

                        const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                        const std::optional<RemoteMousePosition> position = remote_mouse_position(
                            event.button.x,
                            event.button.y,
                            target,
                            texture_width,
                            texture_height);
                        if (position) {
                            send_mouse_report(
                                *state,
                                mouse_buttons,
                                *position,
                                texture_width,
                                texture_height,
                                0,
                                last_relative_mouse_position,
                                mouse_sequence,
                                options.login.verbose);
                        }
                    }
                } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    if (texture_width > 0 && texture_height > 0) {
                        const std::uint64_t ticks = SDL_GetTicks();
                        if (mouse_buttons == 0 &&
                            ticks - last_mouse_motion_ticks < kMouseMotionIntervalMilliseconds) {
                            continue;
                        }

                        const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                        const std::optional<RemoteMousePosition> position = remote_mouse_position(
                            event.motion.x,
                            event.motion.y,
                            target,
                            texture_width,
                            texture_height);
                        if (position) {
                            pending_mouse_motion = *position;
                            pending_mouse_motion_ticks = ticks;
                        }
                    }
                } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                    if (texture_width > 0 && texture_height > 0) {
                        const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                        const std::optional<RemoteMousePosition> position = remote_mouse_position(
                            event.wheel.mouse_x,
                            event.wheel.mouse_y,
                            target,
                            texture_width,
                            texture_height);
                        if (position) {
                            const int wheel = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
                                ? static_cast<int>(-event.wheel.y)
                                : static_cast<int>(event.wheel.y);
                            send_mouse_report(
                                *state,
                                mouse_buttons,
                                *position,
                                texture_width,
                                texture_height,
                                wheel,
                                last_relative_mouse_position,
                                mouse_sequence,
                                options.login.verbose);
                        }
                    }
                }
            }

            if (pending_mouse_motion) {
                send_mouse_report(
                    *state,
                    mouse_buttons,
                    *pending_mouse_motion,
                    texture_width,
                    texture_height,
                    0,
                    last_relative_mouse_position,
                    mouse_sequence,
                    options.login.verbose);
                last_mouse_motion_ticks = pending_mouse_motion_ticks;
            }

            bool cursor_texture_dirty = false;
            if (std::optional<SharedCursor> cursor = take_latest_megarac_view_cursor(*state, last_cursor_sequence)) {
                hardware_cursor = std::move(*cursor);
                last_cursor_sequence = hardware_cursor.sequence;
                has_hardware_cursor = true;
                cursor_texture_dirty = true;
            }

            const std::optional<MegaracViewFrame> frame = take_latest_megarac_view_frame(*state, last_sequence);
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
                        ("hitsc - " + options.login.base_url.host + " - "
                         + std::to_string(frame->width) + "x" + std::to_string(frame->height))
                            .c_str());
                }

                if (!SDL_UpdateTexture(texture, nullptr, frame->rgba.data(), frame->width * 4)) {
                    throw_sdl_error("SDL_UpdateTexture");
                }
                latest_frame_rgba = frame->rgba;
                cursor_texture_dirty = has_hardware_cursor;

                ++presented_frames;
                if (options.login.verbose && (presented_frames <= 20 || presented_frames % 60 == 0)) {
                    log_info() << "presented frame #" << presented_frames
                               << " sequence=" << frame->sequence
                               << " avg-rgb=" << sampled_average_rgb(frame->rgba);
                }
            }

            if (cursor_texture_dirty && has_hardware_cursor) {
                const CursorImage cursor_image = make_cursor_image(
                    hardware_cursor,
                    latest_frame_rgba,
                    texture_width,
                    texture_height);
                update_cursor_texture(
                    renderer,
                    cursor_texture,
                    cursor_texture_width,
                    cursor_texture_height,
                    cursor_image);
            }

            SDL_SetRenderDrawColor(renderer, 12, 14, 18, 255);
            SDL_RenderClear(renderer);
            if (texture != nullptr) {
                const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                SDL_RenderTexture(renderer, texture, nullptr, &target);
                if (cursor_texture != nullptr && has_hardware_cursor && hardware_cursor.visible) {
                    const float scale_x = target.w / static_cast<float>(texture_width);
                    const float scale_y = target.h / static_cast<float>(texture_height);
                    SDL_FRect cursor_target{};
                    cursor_target.x = target.x + static_cast<float>(hardware_cursor.x) * scale_x;
                    cursor_target.y = target.y + static_cast<float>(hardware_cursor.y) * scale_y;
                    cursor_target.w = static_cast<float>(cursor_texture_width) * scale_x;
                    cursor_target.h = static_cast<float>(cursor_texture_height) * scale_y;
                    SDL_RenderTexture(renderer, cursor_texture, nullptr, &cursor_target);
                }
            }
            SDL_RenderPresent(renderer);

            const std::uint64_t ticks = SDL_GetTicks();
            if (texture == nullptr && ticks - last_status_tick > 1000) {
                last_status_tick = ticks;
                const MegaracViewStatusSnapshot snapshot = megarac_view_status_snapshot(*state);
                SDL_SetWindowTitle(window, ("hitsc - " + snapshot.status).c_str());
            }
            SDL_Delay(16);
        }
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "megarac view ui thread");
        stop_network_thread(*state, *stop_requested, network_thread, *network_done);
        if (cursor_texture != nullptr) {
            SDL_DestroyTexture(cursor_texture);
        }
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

    stop_network_thread(*state, *stop_requested, network_thread, *network_done);

    if (cursor_texture != nullptr) {
        SDL_DestroyTexture(cursor_texture);
    }
    if (texture != nullptr) {
        SDL_DestroyTexture(texture);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

} // namespace hitsc
