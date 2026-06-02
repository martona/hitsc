#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace hitsc {

// Shared KVM input plumbing. Everything that is *accidentally* identical across
// the backends (SDL event decode, coordinate mapping, motion throttle, button
// and key state tracking, mouse capture, release-on-focus-loss) lives in
// KvmInputController. Each backend supplies a small KvmInputEncoder that turns
// the canonical input state into its own wire format -- the only part that is
// genuinely protocol-specific.

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
    const SDL_FRect& target,
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
    SDL_FRect target{};
};

struct PointerState {
    NormalizedPoint position;       // always within the target rect
    int frame_width = 0;
    int frame_height = 0;
    std::uint32_t buttons = 0;      // bit (1u << sdl_button) set per held button
};

struct PointerChange {
    enum class Kind { Move, Button, Wheel };
    Kind kind = Kind::Move;
    std::uint8_t button = 0;        // Button: the SDL button index that changed
    bool pressed = false;           // Button: down (true) or up (false)
    float wheel_x = 0.0f;           // Wheel: flip-normalized horizontal delta
    float wheel_y = 0.0f;           // Wheel: flip-normalized vertical delta
};

using KeyDownState = std::array<bool, SDL_SCANCODE_COUNT>;

struct KeyboardState {
    const KeyDownState& down;       // full set of currently-pressed scancodes
};

struct KeyChange {
    std::vector<SDL_Scancode> pressed;   // newly down (usually one)
    std::vector<SDL_Scancode> released;  // newly up (release-all fills this)
};

// The protocol-essential seam. A backend implements this to encode canonical
// input state into its wire packets.
class KvmInputEncoder {
public:
    virtual ~KvmInputEncoder() = default;

    virtual bool accepts_button(std::uint8_t sdl_button) const = 0;
    virtual bool accepts_key(SDL_Scancode scancode) const = 0;

    virtual void encode_pointer(const PointerState& state, const PointerChange& change) = 0;
    virtual void encode_keyboard(const KeyboardState& state, const KeyChange& change) = 0;
};

// Owns all the shared input machinery. The view forwards SDL events here and
// supplies the current frame geometry; the controller drives the encoder.
class KvmInputController {
public:
    KvmInputController(
        KvmInputEncoder& encoder,
        std::function<std::optional<FrameGeometry>()> frame_geometry);

    KvmInputController(const KvmInputController&) = delete;
    KvmInputController& operator=(const KvmInputController&) = delete;

    void handle_event(const SDL_Event& event);
    void release_all_keys();   // on focus loss
    void reset();              // on close/cleanup: drop capture and button state

private:
    void handle_button(const SDL_MouseButtonEvent& button, bool down);
    void handle_motion(const SDL_MouseMotionEvent& motion);
    void handle_wheel(const SDL_MouseWheelEvent& wheel);
    void handle_key(const SDL_KeyboardEvent& key, bool down);

    bool any_button_down() const { return buttons_ != 0; }

    KvmInputEncoder& encoder_;
    std::function<std::optional<FrameGeometry>()> frame_geometry_;
    std::uint32_t buttons_ = 0;
    KeyDownState key_down_{};
    std::uint64_t last_motion_ticks_ = 0;
};

} // namespace hitsc
