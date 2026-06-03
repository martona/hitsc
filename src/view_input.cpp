#include "view_input.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hitsc {

std::optional<NormalizedPoint> target_normalized_point(
    float window_x,
    float window_y,
    const SDL_FRect& target,
    bool clamp_to_target)
{
    if (target.w <= 0.0f || target.h <= 0.0f) {
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
    return NormalizedPoint{
        (static_cast<double>(clamped_x) - static_cast<double>(target.x)) / static_cast<double>(target.w),
        (static_cast<double>(clamped_y) - static_cast<double>(target.y)) / static_cast<double>(target.h),
    };
}

FramePixel to_frame_pixel(NormalizedPoint point, int frame_width, int frame_height)
{
    return FramePixel{
        std::clamp(static_cast<int>(std::floor(point.x * frame_width + 0.5)), 0, frame_width),
        std::clamp(static_cast<int>(std::floor(point.y * frame_height + 0.5)), 0, frame_height),
    };
}

bool mouse_motion_throttled(
    std::uint64_t now_ticks,
    std::uint64_t last_motion_ticks,
    bool drag_active)
{
    return !drag_active && now_ticks - last_motion_ticks < kMouseMotionIntervalMilliseconds;
}

KvmInputController::KvmInputController(
    KvmInputEncoder& encoder,
    std::function<std::optional<FrameGeometry>()> frame_geometry)
    : encoder_(encoder)
    , frame_geometry_(std::move(frame_geometry))
{
}

void KvmInputController::handle_event(const SDL_Event& event)
{
    // The only place SDL event types survive: translate into the backend-neutral
    // Kvm input structs and dispatch. (Replaced wholesale when the Qt event
    // source lands; the handlers below are already SDL-free.)
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        handle_key(KvmKeyEvent{static_cast<KvmScancode>(event.key.scancode), true, event.key.repeat});
        break;
    case SDL_EVENT_KEY_UP:
        handle_key(KvmKeyEvent{static_cast<KvmScancode>(event.key.scancode), false, event.key.repeat});
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        handle_button(KvmPointerButton{
            static_cast<KvmMouseButton>(event.button.button), true, {event.button.x, event.button.y}});
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        handle_button(KvmPointerButton{
            static_cast<KvmMouseButton>(event.button.button), false, {event.button.x, event.button.y}});
        break;
    case SDL_EVENT_MOUSE_MOTION:
        handle_motion(KvmPointerMotion{{event.motion.x, event.motion.y}});
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        const float dx = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.x : event.wheel.x;
        const float dy = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        handle_wheel(KvmPointerWheel{dx, dy, {event.wheel.mouse_x, event.wheel.mouse_y}});
        break;
    }
    default:
        break;
    }
}

void KvmInputController::handle_button(const KvmPointerButton& button)
{
    if (!encoder_.accepts_button(button.button)) {
        return;
    }

    const std::optional<FrameGeometry> frame = frame_geometry_();
    if (!frame) {
        return;
    }

    const bool drag_active = any_button_down();
    const std::optional<NormalizedPoint> position =
        target_normalized_point(button.pos.x, button.pos.y, frame->target, drag_active || !button.down);
    if (!position) {
        return;
    }

    const std::uint32_t bit = 1u << static_cast<std::uint8_t>(button.button);
    if (button.down) {
        buttons_ |= bit;
    } else {
        buttons_ &= ~bit;
    }
    SDL_CaptureMouse(any_button_down());

    encoder_.encode_pointer(
        PointerState{*position, frame->width, frame->height, buttons_},
        PointerChange{PointerChange::Kind::Button, button.button, button.down, 0.0f, 0.0f});
}

void KvmInputController::handle_motion(const KvmPointerMotion& motion)
{
    const std::optional<FrameGeometry> frame = frame_geometry_();
    if (!frame) {
        return;
    }

    const bool drag_active = any_button_down();
    const std::uint64_t ticks = SDL_GetTicks();
    if (mouse_motion_throttled(ticks, last_motion_ticks_, drag_active)) {
        return;
    }

    const std::optional<NormalizedPoint> position =
        target_normalized_point(motion.pos.x, motion.pos.y, frame->target, drag_active);
    if (!position) {
        return;
    }

    encoder_.encode_pointer(
        PointerState{*position, frame->width, frame->height, buttons_},
        PointerChange{PointerChange::Kind::Move});
    last_motion_ticks_ = ticks;
}

void KvmInputController::handle_wheel(const KvmPointerWheel& wheel)
{
    const std::optional<FrameGeometry> frame = frame_geometry_();
    if (!frame) {
        return;
    }

    const bool drag_active = any_button_down();
    const std::optional<NormalizedPoint> position =
        target_normalized_point(wheel.pos.x, wheel.pos.y, frame->target, drag_active);
    if (!position) {
        return;
    }

    PointerChange change{PointerChange::Kind::Wheel};
    change.wheel_x = wheel.dx;
    change.wheel_y = wheel.dy;
    encoder_.encode_pointer(
        PointerState{*position, frame->width, frame->height, buttons_},
        change);
}

void KvmInputController::handle_key(const KvmKeyEvent& key)
{
    if (key.down && key.repeat) {
        return;
    }
    if (!encoder_.accepts_key(key.scancode)) {
        return;
    }

    const auto index = static_cast<std::size_t>(key.scancode);
    if (index >= key_down_.size()) {
        return;
    }
    if (key_down_[index] == key.down) {
        return;
    }
    key_down_[index] = key.down;

    KeyChange change;
    if (key.down) {
        change.pressed.push_back(key.scancode);
    } else {
        change.released.push_back(key.scancode);
    }
    encoder_.encode_keyboard(KeyboardState{key_down_}, change);
}

void KvmInputController::release_all_keys()
{
    KeyChange change;
    for (std::size_t scancode = 0; scancode < key_down_.size(); ++scancode) {
        if (key_down_[scancode]) {
            change.released.push_back(static_cast<KvmScancode>(scancode));
            key_down_[scancode] = false;
        }
    }

    if (!change.released.empty()) {
        encoder_.encode_keyboard(KeyboardState{key_down_}, change);
    }
}

void KvmInputController::reset()
{
    buttons_ = 0;
    SDL_CaptureMouse(false);
}

} // namespace hitsc
