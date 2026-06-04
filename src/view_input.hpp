#pragma once

#include "view_input_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace hitsc {

// Shared KVM input plumbing. Everything that is *accidentally* identical across
// the backends (coordinate mapping, motion throttle, button and key state
// tracking, release-on-focus-loss) lives in KvmInputController. Each backend
// supplies a small KvmInputEncoder that turns the canonical input state into its
// own wire format -- the only part that is genuinely protocol-specific.

struct NormalizedPoint {
    double x = 0.0;
    double y = 0.0;
};

struct FramePixel {
    int x = 0;
    int y = 0;
};

// Map window coordinates onto the target rectangle and normalize to [0, 1].
// Returns nullopt when the point is outside the target and clamping is off.
std::optional<NormalizedPoint> target_normalized_point(
    float window_x,
    float window_y,
    const TargetRect& target,
    bool clamp_to_target);

// Scale a normalized point to integer frame pixels, rounded and clamped.
FramePixel to_frame_pixel(NormalizedPoint point, int frame_width, int frame_height);

inline constexpr std::uint64_t kMouseMotionIntervalMilliseconds = 8;

// Drop mouse-motion events that arrive faster than the interval while no button
// is held (dragging always passes through).
bool mouse_motion_throttled(
    std::uint64_t now_ticks,
    std::uint64_t last_motion_ticks,
    bool drag_active);

// Current rendered frame, supplied by the view per event (nullopt = no frame to
// map onto yet, so mouse events are ignored).
struct FrameGeometry {
    int width = 0;
    int height = 0;
    TargetRect target{};
};

struct PointerState {
    NormalizedPoint position;       // always within the target rect
    int frame_width = 0;
    int frame_height = 0;
    std::uint32_t buttons = 0;      // bit (1u << button value) set per held button
};

struct PointerChange {
    enum class Kind { Move, Button, Wheel };
    Kind kind = Kind::Move;
    KvmMouseButton button = KvmMouseButton::LEFT;  // Button: the button that changed
    bool pressed = false;           // Button: down (true) or up (false)
    float wheel_x = 0.0f;           // Wheel: flip-normalized horizontal delta
    float wheel_y = 0.0f;           // Wheel: flip-normalized vertical delta
};

using KeyDownState = std::array<bool, kKvmScancodeCount>;

struct KeyboardState {
    const KeyDownState& down;       // full set of currently-pressed scancodes
};

struct KeyChange {
    std::vector<KvmScancode> pressed;   // newly down (usually one)
    std::vector<KvmScancode> released;  // newly up (release-all fills this)
};

// The protocol-essential seam. A backend implements this to encode canonical
// input state into its wire packets.
class KvmInputEncoder {
public:
    virtual ~KvmInputEncoder() = default;

    virtual bool accepts_button(KvmMouseButton button) const = 0;
    virtual bool accepts_key(KvmScancode scancode) const = 0;

    virtual void encode_pointer(const PointerState& state, const PointerChange& change) = 0;
    virtual void encode_keyboard(const KeyboardState& state, const KeyChange& change) = 0;
};

// Owns all the shared input machinery. The Qt viewer feeds decoded input here
// and supplies the current frame geometry; the controller drives the encoder.
class KvmInputController {
public:
    KvmInputController(
        KvmInputEncoder& encoder,
        std::function<std::optional<FrameGeometry>()> frame_geometry);

    KvmInputController(const KvmInputController&) = delete;
    KvmInputController& operator=(const KvmInputController&) = delete;

    void release_all_keys();   // on focus loss
    void reset();              // on close/cleanup: drop button state

    // Input entry points: the Qt viewer feeds already-decoded canonical events,
    // which run through the same throttle/state logic as the rest of the class.
    void feed_pointer_button(const KvmPointerButton& button) { handle_button(button); }
    void feed_pointer_motion(const KvmPointerMotion& motion) { handle_motion(motion); }
    void feed_pointer_wheel(const KvmPointerWheel& wheel) { handle_wheel(wheel); }
    void feed_key(const KvmKeyEvent& key) { handle_key(key); }

    // Install the frame-geometry source: maps pointer coordinates against the Qt
    // surface (its current size + the centred frame rect).
    void set_frame_geometry_source(std::function<std::optional<FrameGeometry>()> source)
    {
        frame_geometry_ = std::move(source);
    }

private:
    void handle_button(const KvmPointerButton& button);
    void handle_motion(const KvmPointerMotion& motion);
    void handle_wheel(const KvmPointerWheel& wheel);
    void handle_key(const KvmKeyEvent& key);

    bool any_button_down() const { return buttons_ != 0; }

    KvmInputEncoder& encoder_;
    std::function<std::optional<FrameGeometry>()> frame_geometry_;
    std::uint32_t buttons_ = 0;
    KeyDownState key_down_{};
    std::uint64_t last_motion_ticks_ = 0;
};

} // namespace hitsc
