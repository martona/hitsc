#pragma once

#include <cstddef>
#include <cstdint>

namespace hitsc {

// Upper bound for indexing a key-down state array by scancode value. Covers the
// whole USB HID keyboard page with margin (modifiers top out at 231); the input
// controller bounds-checks before indexing, so this only needs to be >= any
// scancode we actually track.
inline constexpr std::size_t kKvmScancodeCount = 512;

// Keyboard scancodes, as USB HID Usage Table page 0x07 codes.
//
// These values are IDENTICAL to the SDL_Scancode values they replace, and that
// is load-bearing: the ATEN and MegaRAC encoders send the raw numeric value to
// the guest as its HID usage (SDL mirrors the HID table, so SDL_SCANCODE_A == 4
// == HID usage "a"). Keep these values in lockstep with the USB HID table.
//
// The enumerator names also match SDL's (sans prefix) so the SDL->Kvm migration
// could be a mechanical `SDL_SCANCODE_` -> `KvmScancode::` replace; the only
// exception is the top-row digits, which can't be bare numbers (DIGIT_n here).
enum class KvmScancode : std::uint16_t {
    UNKNOWN = 0,

    A = 4, B = 5, C = 6, D = 7, E = 8, F = 9, G = 10, H = 11, I = 12, J = 13,
    K = 14, L = 15, M = 16, N = 17, O = 18, P = 19, Q = 20, R = 21, S = 22,
    T = 23, U = 24, V = 25, W = 26, X = 27, Y = 28, Z = 29,

    DIGIT_1 = 30, DIGIT_2 = 31, DIGIT_3 = 32, DIGIT_4 = 33, DIGIT_5 = 34,
    DIGIT_6 = 35, DIGIT_7 = 36, DIGIT_8 = 37, DIGIT_9 = 38, DIGIT_0 = 39,

    RETURN = 40, ESCAPE = 41, BACKSPACE = 42, TAB = 43, SPACE = 44,
    MINUS = 45, EQUALS = 46, LEFTBRACKET = 47, RIGHTBRACKET = 48, BACKSLASH = 49,
    NONUSHASH = 50, SEMICOLON = 51, APOSTROPHE = 52, GRAVE = 53,
    COMMA = 54, PERIOD = 55, SLASH = 56, CAPSLOCK = 57,

    F1 = 58, F2 = 59, F3 = 60, F4 = 61, F5 = 62, F6 = 63,
    F7 = 64, F8 = 65, F9 = 66, F10 = 67, F11 = 68, F12 = 69,

    PRINTSCREEN = 70, SCROLLLOCK = 71, PAUSE = 72, INSERT = 73, HOME = 74,
    // DELETE_KEY, not DELETE: <winnt.h> #defines DELETE as an access-mask macro.
    PAGEUP = 75, DELETE_KEY = 76, END = 77, PAGEDOWN = 78,
    RIGHT = 79, LEFT = 80, DOWN = 81, UP = 82,

    NUMLOCKCLEAR = 83, KP_DIVIDE = 84, KP_MULTIPLY = 85, KP_MINUS = 86, KP_PLUS = 87,
    KP_ENTER = 88, KP_1 = 89, KP_2 = 90, KP_3 = 91, KP_4 = 92, KP_5 = 93,
    KP_6 = 94, KP_7 = 95, KP_8 = 96, KP_9 = 97, KP_0 = 98, KP_PERIOD = 99,

    NONUSBACKSLASH = 100, APPLICATION = 101, POWER = 102, KP_EQUALS = 103,
    F13 = 104, F14 = 105, F15 = 106, F16 = 107, F17 = 108, F18 = 109,
    F19 = 110, F20 = 111, F21 = 112, F22 = 113, F23 = 114, F24 = 115,

    LCTRL = 224, LSHIFT = 225, LALT = 226, LGUI = 227,
    RCTRL = 228, RSHIFT = 229, RALT = 230, RGUI = 231,
};

// Mouse buttons, matching SDL_BUTTON_* values (1-based) and names (sans prefix);
// the encoders branch on these exact values (e.g. X1/X2 -> "up"/"down").
enum class KvmMouseButton : std::uint8_t {
    LEFT = 1,
    MIDDLE = 2,
    RIGHT = 3,
    X1 = 4,
    X2 = 5,
};

// Window-relative pointer position, in device pixels of the render surface.
struct KvmPointerPos {
    float x = 0.0f;
    float y = 0.0f;
};

struct KvmKeyEvent {
    KvmScancode scancode = KvmScancode::UNKNOWN;
    bool down = false;
    bool repeat = false;
};

struct KvmPointerMotion {
    KvmPointerPos pos;
};

struct KvmPointerButton {
    KvmMouseButton button = KvmMouseButton::LEFT;
    bool down = false;
    KvmPointerPos pos;
};

struct KvmPointerWheel {
    float dx = 0.0f;   // +right
    float dy = 0.0f;   // +up
    KvmPointerPos pos;
};

} // namespace hitsc
