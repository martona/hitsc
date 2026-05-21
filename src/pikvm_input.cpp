#include "pikvm_input.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace hitsc {
namespace {

constexpr int kPikvmMouseMin = -32768;
constexpr int kPikvmMouseMax = 32767;

void append_i16_be(std::vector<std::uint8_t>& bytes, int value)
{
    const auto clamped = static_cast<std::int16_t>(
        std::clamp(value, kPikvmMouseMin, kPikvmMouseMax));
    const auto raw = static_cast<std::uint16_t>(clamped);
    bytes.push_back(static_cast<std::uint8_t>((raw >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(raw & 0xFF));
}

std::int8_t clamp_i8(int value)
{
    return static_cast<std::int8_t>(
        std::clamp(
            value,
            static_cast<int>(std::numeric_limits<std::int8_t>::min()),
            static_cast<int>(std::numeric_limits<std::int8_t>::max())));
}

void append_ascii(std::vector<std::uint8_t>& bytes, std::string_view text)
{
    for (const char ch : text) {
        bytes.push_back(static_cast<std::uint8_t>(ch));
    }
}

} // namespace

std::optional<std::string_view> pikvm_key_code_from_sdl_scancode(SDL_Scancode scancode)
{
    switch (scancode) {
    case SDL_SCANCODE_A:
        return "KeyA";
    case SDL_SCANCODE_B:
        return "KeyB";
    case SDL_SCANCODE_C:
        return "KeyC";
    case SDL_SCANCODE_D:
        return "KeyD";
    case SDL_SCANCODE_E:
        return "KeyE";
    case SDL_SCANCODE_F:
        return "KeyF";
    case SDL_SCANCODE_G:
        return "KeyG";
    case SDL_SCANCODE_H:
        return "KeyH";
    case SDL_SCANCODE_I:
        return "KeyI";
    case SDL_SCANCODE_J:
        return "KeyJ";
    case SDL_SCANCODE_K:
        return "KeyK";
    case SDL_SCANCODE_L:
        return "KeyL";
    case SDL_SCANCODE_M:
        return "KeyM";
    case SDL_SCANCODE_N:
        return "KeyN";
    case SDL_SCANCODE_O:
        return "KeyO";
    case SDL_SCANCODE_P:
        return "KeyP";
    case SDL_SCANCODE_Q:
        return "KeyQ";
    case SDL_SCANCODE_R:
        return "KeyR";
    case SDL_SCANCODE_S:
        return "KeyS";
    case SDL_SCANCODE_T:
        return "KeyT";
    case SDL_SCANCODE_U:
        return "KeyU";
    case SDL_SCANCODE_V:
        return "KeyV";
    case SDL_SCANCODE_W:
        return "KeyW";
    case SDL_SCANCODE_X:
        return "KeyX";
    case SDL_SCANCODE_Y:
        return "KeyY";
    case SDL_SCANCODE_Z:
        return "KeyZ";

    case SDL_SCANCODE_1:
        return "Digit1";
    case SDL_SCANCODE_2:
        return "Digit2";
    case SDL_SCANCODE_3:
        return "Digit3";
    case SDL_SCANCODE_4:
        return "Digit4";
    case SDL_SCANCODE_5:
        return "Digit5";
    case SDL_SCANCODE_6:
        return "Digit6";
    case SDL_SCANCODE_7:
        return "Digit7";
    case SDL_SCANCODE_8:
        return "Digit8";
    case SDL_SCANCODE_9:
        return "Digit9";
    case SDL_SCANCODE_0:
        return "Digit0";

    case SDL_SCANCODE_RETURN:
        return "Enter";
    case SDL_SCANCODE_ESCAPE:
        return "Escape";
    case SDL_SCANCODE_BACKSPACE:
        return "Backspace";
    case SDL_SCANCODE_TAB:
        return "Tab";
    case SDL_SCANCODE_SPACE:
        return "Space";
    case SDL_SCANCODE_MINUS:
        return "Minus";
    case SDL_SCANCODE_EQUALS:
        return "Equal";
    case SDL_SCANCODE_LEFTBRACKET:
        return "BracketLeft";
    case SDL_SCANCODE_RIGHTBRACKET:
        return "BracketRight";
    case SDL_SCANCODE_BACKSLASH:
        return "Backslash";
    case SDL_SCANCODE_NONUSHASH:
        return "IntlBackslash";
    case SDL_SCANCODE_SEMICOLON:
        return "Semicolon";
    case SDL_SCANCODE_APOSTROPHE:
        return "Quote";
    case SDL_SCANCODE_GRAVE:
        return "Backquote";
    case SDL_SCANCODE_COMMA:
        return "Comma";
    case SDL_SCANCODE_PERIOD:
        return "Period";
    case SDL_SCANCODE_SLASH:
        return "Slash";
    case SDL_SCANCODE_CAPSLOCK:
        return "CapsLock";

    case SDL_SCANCODE_F1:
        return "F1";
    case SDL_SCANCODE_F2:
        return "F2";
    case SDL_SCANCODE_F3:
        return "F3";
    case SDL_SCANCODE_F4:
        return "F4";
    case SDL_SCANCODE_F5:
        return "F5";
    case SDL_SCANCODE_F6:
        return "F6";
    case SDL_SCANCODE_F7:
        return "F7";
    case SDL_SCANCODE_F8:
        return "F8";
    case SDL_SCANCODE_F9:
        return "F9";
    case SDL_SCANCODE_F10:
        return "F10";
    case SDL_SCANCODE_F11:
        return "F11";
    case SDL_SCANCODE_F12:
        return "F12";

    case SDL_SCANCODE_PRINTSCREEN:
        return "PrintScreen";
    case SDL_SCANCODE_SCROLLLOCK:
        return "ScrollLock";
    case SDL_SCANCODE_PAUSE:
        return "Pause";
    case SDL_SCANCODE_INSERT:
        return "Insert";
    case SDL_SCANCODE_HOME:
        return "Home";
    case SDL_SCANCODE_PAGEUP:
        return "PageUp";
    case SDL_SCANCODE_DELETE:
        return "Delete";
    case SDL_SCANCODE_END:
        return "End";
    case SDL_SCANCODE_PAGEDOWN:
        return "PageDown";
    case SDL_SCANCODE_RIGHT:
        return "ArrowRight";
    case SDL_SCANCODE_LEFT:
        return "ArrowLeft";
    case SDL_SCANCODE_DOWN:
        return "ArrowDown";
    case SDL_SCANCODE_UP:
        return "ArrowUp";

    case SDL_SCANCODE_NUMLOCKCLEAR:
        return "NumLock";
    case SDL_SCANCODE_KP_DIVIDE:
        return "NumpadDivide";
    case SDL_SCANCODE_KP_MULTIPLY:
        return "NumpadMultiply";
    case SDL_SCANCODE_KP_MINUS:
        return "NumpadSubtract";
    case SDL_SCANCODE_KP_PLUS:
        return "NumpadAdd";
    case SDL_SCANCODE_KP_ENTER:
        return "NumpadEnter";
    case SDL_SCANCODE_KP_1:
        return "Numpad1";
    case SDL_SCANCODE_KP_2:
        return "Numpad2";
    case SDL_SCANCODE_KP_3:
        return "Numpad3";
    case SDL_SCANCODE_KP_4:
        return "Numpad4";
    case SDL_SCANCODE_KP_5:
        return "Numpad5";
    case SDL_SCANCODE_KP_6:
        return "Numpad6";
    case SDL_SCANCODE_KP_7:
        return "Numpad7";
    case SDL_SCANCODE_KP_8:
        return "Numpad8";
    case SDL_SCANCODE_KP_9:
        return "Numpad9";
    case SDL_SCANCODE_KP_0:
        return "Numpad0";
    case SDL_SCANCODE_KP_PERIOD:
        return "NumpadDecimal";
    case SDL_SCANCODE_NONUSBACKSLASH:
        return "IntlBackslash";
    case SDL_SCANCODE_APPLICATION:
        return "ContextMenu";
    case SDL_SCANCODE_KP_EQUALS:
        return "NumpadEqual";

    case SDL_SCANCODE_F13:
        return "F13";
    case SDL_SCANCODE_F14:
        return "F14";
    case SDL_SCANCODE_F15:
        return "F15";
    case SDL_SCANCODE_F16:
        return "F16";
    case SDL_SCANCODE_F17:
        return "F17";
    case SDL_SCANCODE_F18:
        return "F18";
    case SDL_SCANCODE_F19:
        return "F19";
    case SDL_SCANCODE_F20:
        return "F20";
    case SDL_SCANCODE_F21:
        return "F21";
    case SDL_SCANCODE_F22:
        return "F22";
    case SDL_SCANCODE_F23:
        return "F23";
    case SDL_SCANCODE_F24:
        return "F24";

    case SDL_SCANCODE_LCTRL:
        return "ControlLeft";
    case SDL_SCANCODE_LSHIFT:
        return "ShiftLeft";
    case SDL_SCANCODE_LALT:
        return "AltLeft";
    case SDL_SCANCODE_LGUI:
        return "MetaLeft";
    case SDL_SCANCODE_RCTRL:
        return "ControlRight";
    case SDL_SCANCODE_RSHIFT:
        return "ShiftRight";
    case SDL_SCANCODE_RALT:
        return "AltRight";
    case SDL_SCANCODE_RGUI:
        return "MetaRight";

    default:
        return std::nullopt;
    }
}

std::optional<std::string_view> pikvm_mouse_button_from_sdl_button(std::uint8_t button)
{
    switch (button) {
    case SDL_BUTTON_LEFT:
        return "left";
    case SDL_BUTTON_MIDDLE:
        return "middle";
    case SDL_BUTTON_RIGHT:
        return "right";
    case SDL_BUTTON_X1:
        return "up";
    case SDL_BUTTON_X2:
        return "down";
    default:
        return std::nullopt;
    }
}

PikvmAbsoluteMousePosition make_pikvm_absolute_mouse_position(double normalized_x, double normalized_y)
{
    normalized_x = std::clamp(normalized_x, 0.0, 1.0);
    normalized_y = std::clamp(normalized_y, 0.0, 1.0);

    constexpr double range = static_cast<double>(kPikvmMouseMax - kPikvmMouseMin);
    return PikvmAbsoluteMousePosition{
        static_cast<int>(std::lround(static_cast<double>(kPikvmMouseMin) + normalized_x * range)),
        static_cast<int>(std::lround(static_cast<double>(kPikvmMouseMin) + normalized_y * range)),
    };
}

std::vector<std::uint8_t> make_pikvm_key_packet(
    std::string_view code,
    bool pressed,
    bool finish)
{
    std::vector<std::uint8_t> packet;
    packet.reserve(2 + code.size());
    packet.push_back(1);
    packet.push_back(
        static_cast<std::uint8_t>((pressed ? 0x01 : 0x00) | (finish ? 0x02 : 0x00)));
    append_ascii(packet, code);
    return packet;
}

std::vector<std::uint8_t> make_pikvm_mouse_button_packet(
    std::string_view button,
    bool pressed)
{
    std::vector<std::uint8_t> packet;
    packet.reserve(2 + button.size());
    packet.push_back(2);
    packet.push_back(static_cast<std::uint8_t>(pressed ? 1U : 0U));
    append_ascii(packet, button);
    return packet;
}

std::vector<std::uint8_t> make_pikvm_mouse_move_packet(const PikvmAbsoluteMousePosition& position)
{
    std::vector<std::uint8_t> packet;
    packet.reserve(5);
    packet.push_back(3);
    append_i16_be(packet, position.x);
    append_i16_be(packet, position.y);
    return packet;
}

std::vector<std::uint8_t> make_pikvm_mouse_wheel_packet(
    int delta_x,
    int delta_y,
    bool squash)
{
    std::vector<std::uint8_t> packet;
    packet.reserve(4);
    packet.push_back(5);
    packet.push_back(static_cast<std::uint8_t>(squash ? 1U : 0U));
    packet.push_back(static_cast<std::uint8_t>(clamp_i8(delta_x)));
    packet.push_back(static_cast<std::uint8_t>(clamp_i8(delta_y)));
    return packet;
}

} // namespace hitsc
