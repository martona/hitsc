#include "megarac_view.hpp"

#include "aspeed_view_renderer.hpp"
#include "diagnostics.hpp"
#include "log.hpp"
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
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace hitsc {
namespace {

constexpr std::uint64_t kMouseMotionIntervalMilliseconds = 8;

constexpr std::uint16_t kCmdSendHidPacket = command_value(MegaracCommand::SendHidPacket);
constexpr std::uint16_t kCmdGetFullScreen = command_value(MegaracCommand::GetFullScreen);
using KeyboardKeySlots = MegaracKeyboardKeySlots;

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

std::optional<RemoteMousePosition> remote_mouse_position(
    float window_x,
    float window_y,
    const SDL_FRect& target,
    int frame_width,
    int frame_height,
    bool clamp_to_target)
{
    if (frame_width <= 0 || frame_height <= 0 || target.w <= 0.0f || target.h <= 0.0f) {
        return std::nullopt;
    }
    const bool inside =
        window_x >= target.x
        && window_y >= target.y
        && window_x <= target.x + target.w
        && window_y <= target.y + target.h;
    if (!inside && !clamp_to_target) {
        return std::nullopt;
    }

    const float clamped_x = std::clamp(window_x, target.x, target.x + target.w);
    const float clamped_y = std::clamp(window_y, target.y, target.y + target.h);
    const double normalized_x =
        (static_cast<double>(clamped_x) - static_cast<double>(target.x)) / static_cast<double>(target.w);
    const double normalized_y =
        (static_cast<double>(clamped_y) - static_cast<double>(target.y)) / static_cast<double>(target.h);
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
    state.input.enqueue(MegaracInputWork{
        kCmdSendHidPacket,
        make_megarac_keyboard_packet(MegaracKeyboardReport{modifiers, keys}, sequence++)});
    if (verbose) {
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
    state.input.enqueue(MegaracInputWork{kCmdSendHidPacket, std::move(packet)});
    if (verbose) {
        log_info() << "queued mouse"
                   << " mode=" << mouse_mode
                   << " buttons=" << static_cast<int>(buttons)
                   << " x=" << position.x
                   << " y=" << position.y
                   << " wheel=" << wheel;
    }
}

class MegaracView : public KvmViewBase {
public:
    explicit MegaracView(const MegaracViewOptions& options)
        : MegaracView(options, std::make_shared<MegaracViewSessionState>())
    {
    }

private:
    MegaracView(const MegaracViewOptions& options, std::shared_ptr<MegaracViewSessionState> state)
        : KvmViewBase(*state, options.login.base_url.host, "megarac", [state] {
              state->input.clear();
          })
        , options_(options)
        , state_(std::move(state))
    {
    }

    void start_network(KvmNetworkWorker& network) override
    {
        MegaracViewOptions network_options = options_;
        std::shared_ptr<MegaracViewSessionState> state = state_;
        network.start([network_options, state](std::atomic_bool& stop_requested) {
            run_megarac_view_session(network_options, *state, stop_requested);
        });
    }

    void before_sdl_cleanup() override
    {
        aspeed_.destroy();
    }

    void on_minimized() override
    {
        state_->frames.clear();
        aspeed_.destroy();
        aspeed_.reset_sequences();
    }

    void on_restored() override
    {
        state_->frames.clear();
        aspeed_.reset_sequences();
        state_->input.enqueue(MegaracInputWork{kCmdGetFullScreen, make_simple_packet(kCmdGetFullScreen, 1)});
    }

    void on_focus_lost() override
    {
        if (has_keyboard_state(keyboard_modifiers_, keyboard_keys_)) {
            keyboard_modifiers_ = 0;
            keyboard_keys_.fill(0);
            send_keyboard_report(
                *state_,
                keyboard_modifiers_,
                keyboard_keys_,
                keyboard_sequence_,
                options_.login.vverbose);
        }
    }

    void handle_event(const SDL_Event& event, bool&) override
    {
        const AspeedPresentationSlot* active = aspeed_.active_slot();
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            handle_key_event(event);
        } else if (active != nullptr &&
                   (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                    event.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
            handle_mouse_button_event(event, *active);
        } else if (active != nullptr && event.type == SDL_EVENT_MOUSE_MOTION) {
            handle_mouse_motion_event(event, *active);
        } else if (active != nullptr && event.type == SDL_EVENT_MOUSE_WHEEL) {
            handle_mouse_wheel_event(event, *active);
        }
    }

    void render_visible(bool& render_needed, bool& first_render) override
    {
        bool presented_new_frame = false;
        aspeed_.update(
            renderer(),
            state_->frames,
            state_->cursors,
            "MegaRAC",
            options_.login.vverbose,
            render_needed,
            presented_new_frame);

        if (!render_needed) {
            return;
        }

        clear_background();
        if (const AspeedPresentationSlot* active = aspeed_.active_slot();
            active != nullptr && active->texture != nullptr) {
            aspeed_.render(renderer(), current_target_rect(active->width, active->height));
        }
        present();

        if (presented_new_frame) {
            if (const AspeedPresentationSlot* active = aspeed_.active_slot()) {
                frame_presented(active->width, active->height);
                state_->video_feedback_presented_frames.fetch_add(1, std::memory_order_relaxed);
            }
        }
        first_render = false;
    }

    void handle_key_event(const SDL_Event& event)
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat) {
            return;
        }

        const bool pressed = event.type == SDL_EVENT_KEY_DOWN;
        const std::optional<std::uint8_t> modifier = keyboard_modifier_bit(event.key.scancode);
        bool changed = false;
        if (modifier) {
            if (pressed) {
                changed = (keyboard_modifiers_ & *modifier) == 0;
                keyboard_modifiers_ |= *modifier;
            } else {
                changed = (keyboard_modifiers_ & *modifier) != 0;
                keyboard_modifiers_ &= static_cast<std::uint8_t>(~*modifier);
            }
        } else if (const std::optional<std::uint8_t> usage =
                       keyboard_usage_from_sdl_scancode(event.key.scancode)) {
            changed = set_keyboard_usage(keyboard_keys_, *usage, pressed);
        }

        if (changed) {
            send_keyboard_report(
                *state_,
                keyboard_modifiers_,
                keyboard_keys_,
                keyboard_sequence_,
                options_.login.vverbose);
        }
    }

    void handle_mouse_button_event(const SDL_Event& event, const AspeedPresentationSlot& active)
    {
        const std::uint8_t mask = button_mask_for_sdl_button(event.button.button);
        if (mask == 0) {
            return;
        }

        const SDL_FRect target = current_target_rect(active.width, active.height);
        const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        const bool drag_active = mouse_buttons_ != 0;
        const std::optional<RemoteMousePosition> position = remote_mouse_position(
            event.button.x,
            event.button.y,
            target,
            active.width,
            active.height,
            drag_active || !down);
        if (!position) {
            return;
        }

        if (down) {
            mouse_buttons_ |= mask;
        } else {
            mouse_buttons_ &= static_cast<std::uint8_t>(~mask);
        }
        SDL_CaptureMouse(mouse_buttons_ != 0);
        send_mouse_report(
            *state_,
            mouse_buttons_,
            *position,
            active.width,
            active.height,
            0,
            last_relative_mouse_position_,
            mouse_sequence_,
            options_.login.vverbose);
    }

    void handle_mouse_motion_event(const SDL_Event& event, const AspeedPresentationSlot& active)
    {
        const std::uint64_t ticks = SDL_GetTicks();
        const bool throttled =
            mouse_buttons_ == 0 &&
            ticks - last_mouse_motion_ticks_ < kMouseMotionIntervalMilliseconds;
        if (throttled) {
            return;
        }

        const SDL_FRect target = current_target_rect(active.width, active.height);
        const std::optional<RemoteMousePosition> position = remote_mouse_position(
            event.motion.x,
            event.motion.y,
            target,
            active.width,
            active.height,
            mouse_buttons_ != 0);
        if (position) {
            send_mouse_report(
                *state_,
                mouse_buttons_,
                *position,
                active.width,
                active.height,
                0,
                last_relative_mouse_position_,
                mouse_sequence_,
                options_.login.vverbose);
            last_mouse_motion_ticks_ = ticks;
        }
    }

    void handle_mouse_wheel_event(const SDL_Event& event, const AspeedPresentationSlot& active)
    {
        const SDL_FRect target = current_target_rect(active.width, active.height);
        const std::optional<RemoteMousePosition> position = remote_mouse_position(
            event.wheel.mouse_x,
            event.wheel.mouse_y,
            target,
            active.width,
            active.height,
            mouse_buttons_ != 0);
        if (!position) {
            return;
        }

        const int wheel = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
            ? static_cast<int>(-event.wheel.y)
            : static_cast<int>(event.wheel.y);
        send_mouse_report(
            *state_,
            mouse_buttons_,
            *position,
            active.width,
            active.height,
            wheel,
            last_relative_mouse_position_,
            mouse_sequence_,
            options_.login.vverbose);
    }

    MegaracViewOptions options_;
    std::shared_ptr<MegaracViewSessionState> state_;
    AspeedViewRenderer aspeed_;
    std::uint8_t mouse_buttons_ = 0;
    std::uint32_t mouse_sequence_ = 0;
    std::uint8_t keyboard_modifiers_ = 0;
    KeyboardKeySlots keyboard_keys_{};
    std::uint32_t keyboard_sequence_ = 0;
    std::uint64_t last_mouse_motion_ticks_ = 0;
    std::optional<RemoteMousePosition> last_relative_mouse_position_;
};

} // namespace

void run_megarac_view(const MegaracViewOptions& options)
{
    try {
        MegaracView view(options);
        view.run();
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "megarac view ui thread");
        throw;
    }
}

} // namespace hitsc
