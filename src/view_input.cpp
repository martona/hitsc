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
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        handle_key(event.key, true);
        break;
    case SDL_EVENT_KEY_UP:
        handle_key(event.key, false);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        handle_button(event.button, true);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        handle_button(event.button, false);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        handle_motion(event.motion);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        handle_wheel(event.wheel);
        break;
    default:
        break;
    }
}

void KvmInputController::handle_button(const SDL_MouseButtonEvent& button, bool down)
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
        target_normalized_point(button.x, button.y, frame->target, drag_active || !down);
    if (!position) {
        return;
    }

    const std::uint32_t bit = 1u << button.button;
    if (down) {
        buttons_ |= bit;
    } else {
        buttons_ &= ~bit;
    }
    SDL_CaptureMouse(any_button_down());

    encoder_.encode_pointer(
        PointerState{*position, frame->width, frame->height, buttons_},
        PointerChange{PointerChange::Kind::Button, button.button, down, 0.0f, 0.0f});
}

void KvmInputController::handle_motion(const SDL_MouseMotionEvent& motion)
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
        target_normalized_point(motion.x, motion.y, frame->target, drag_active);
    if (!position) {
        return;
    }

    encoder_.encode_pointer(
        PointerState{*position, frame->width, frame->height, buttons_},
        PointerChange{PointerChange::Kind::Move});
    last_motion_ticks_ = ticks;
}

void KvmInputController::handle_wheel(const SDL_MouseWheelEvent& wheel)
{
    const std::optional<FrameGeometry> frame = frame_geometry_();
    if (!frame) {
        return;
    }

    const bool drag_active = any_button_down();
    const std::optional<NormalizedPoint> position =
        target_normalized_point(wheel.mouse_x, wheel.mouse_y, frame->target, drag_active);
    if (!position) {
        return;
    }

    const float wheel_x = wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -wheel.x : wheel.x;
    const float wheel_y = wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -wheel.y : wheel.y;

    PointerChange change{PointerChange::Kind::Wheel};
    change.wheel_x = wheel_x;
    change.wheel_y = wheel_y;
    encoder_.encode_pointer(
        PointerState{*position, frame->width, frame->height, buttons_},
        change);
}

void KvmInputController::handle_key(const SDL_KeyboardEvent& key, bool down)
{
    if (down && key.repeat) {
        return;
    }
    if (!encoder_.accepts_key(key.scancode)) {
        return;
    }

    const auto index = static_cast<std::size_t>(key.scancode);
    if (index >= key_down_.size()) {
        return;
    }
    if (key_down_[index] == down) {
        return;
    }
    key_down_[index] = down;

    KeyChange change;
    if (down) {
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
            change.released.push_back(static_cast<SDL_Scancode>(scancode));
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
