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

std::optional<std::string_view> pikvm_key_code_from_scancode(KvmScancode scancode)
{
    switch (scancode) {
    case KvmScancode::A:
        return "KeyA";
    case KvmScancode::B:
        return "KeyB";
    case KvmScancode::C:
        return "KeyC";
    case KvmScancode::D:
        return "KeyD";
    case KvmScancode::E:
        return "KeyE";
    case KvmScancode::F:
        return "KeyF";
    case KvmScancode::G:
        return "KeyG";
    case KvmScancode::H:
        return "KeyH";
    case KvmScancode::I:
        return "KeyI";
    case KvmScancode::J:
        return "KeyJ";
    case KvmScancode::K:
        return "KeyK";
    case KvmScancode::L:
        return "KeyL";
    case KvmScancode::M:
        return "KeyM";
    case KvmScancode::N:
        return "KeyN";
    case KvmScancode::O:
        return "KeyO";
    case KvmScancode::P:
        return "KeyP";
    case KvmScancode::Q:
        return "KeyQ";
    case KvmScancode::R:
        return "KeyR";
    case KvmScancode::S:
        return "KeyS";
    case KvmScancode::T:
        return "KeyT";
    case KvmScancode::U:
        return "KeyU";
    case KvmScancode::V:
        return "KeyV";
    case KvmScancode::W:
        return "KeyW";
    case KvmScancode::X:
        return "KeyX";
    case KvmScancode::Y:
        return "KeyY";
    case KvmScancode::Z:
        return "KeyZ";

    case KvmScancode::DIGIT_1:
        return "Digit1";
    case KvmScancode::DIGIT_2:
        return "Digit2";
    case KvmScancode::DIGIT_3:
        return "Digit3";
    case KvmScancode::DIGIT_4:
        return "Digit4";
    case KvmScancode::DIGIT_5:
        return "Digit5";
    case KvmScancode::DIGIT_6:
        return "Digit6";
    case KvmScancode::DIGIT_7:
        return "Digit7";
    case KvmScancode::DIGIT_8:
        return "Digit8";
    case KvmScancode::DIGIT_9:
        return "Digit9";
    case KvmScancode::DIGIT_0:
        return "Digit0";

    case KvmScancode::RETURN:
        return "Enter";
    case KvmScancode::ESCAPE:
        return "Escape";
    case KvmScancode::BACKSPACE:
        return "Backspace";
    case KvmScancode::TAB:
        return "Tab";
    case KvmScancode::SPACE:
        return "Space";
    case KvmScancode::MINUS:
        return "Minus";
    case KvmScancode::EQUALS:
        return "Equal";
    case KvmScancode::LEFTBRACKET:
        return "BracketLeft";
    case KvmScancode::RIGHTBRACKET:
        return "BracketRight";
    case KvmScancode::BACKSLASH:
        return "Backslash";
    case KvmScancode::NONUSHASH:
        return "IntlBackslash";
    case KvmScancode::SEMICOLON:
        return "Semicolon";
    case KvmScancode::APOSTROPHE:
        return "Quote";
    case KvmScancode::GRAVE:
        return "Backquote";
    case KvmScancode::COMMA:
        return "Comma";
    case KvmScancode::PERIOD:
        return "Period";
    case KvmScancode::SLASH:
        return "Slash";
    case KvmScancode::CAPSLOCK:
        return "CapsLock";

    case KvmScancode::F1:
        return "F1";
    case KvmScancode::F2:
        return "F2";
    case KvmScancode::F3:
        return "F3";
    case KvmScancode::F4:
        return "F4";
    case KvmScancode::F5:
        return "F5";
    case KvmScancode::F6:
        return "F6";
    case KvmScancode::F7:
        return "F7";
    case KvmScancode::F8:
        return "F8";
    case KvmScancode::F9:
        return "F9";
    case KvmScancode::F10:
        return "F10";
    case KvmScancode::F11:
        return "F11";
    case KvmScancode::F12:
        return "F12";

    case KvmScancode::PRINTSCREEN:
        return "PrintScreen";
    case KvmScancode::SCROLLLOCK:
        return "ScrollLock";
    case KvmScancode::PAUSE:
        return "Pause";
    case KvmScancode::INSERT:
        return "Insert";
    case KvmScancode::HOME:
        return "Home";
    case KvmScancode::PAGEUP:
        return "PageUp";
    case KvmScancode::DELETE_KEY:
        return "Delete";
    case KvmScancode::END:
        return "End";
    case KvmScancode::PAGEDOWN:
        return "PageDown";
    case KvmScancode::RIGHT:
        return "ArrowRight";
    case KvmScancode::LEFT:
        return "ArrowLeft";
    case KvmScancode::DOWN:
        return "ArrowDown";
    case KvmScancode::UP:
        return "ArrowUp";

    case KvmScancode::NUMLOCKCLEAR:
        return "NumLock";
    case KvmScancode::KP_DIVIDE:
        return "NumpadDivide";
    case KvmScancode::KP_MULTIPLY:
        return "NumpadMultiply";
    case KvmScancode::KP_MINUS:
        return "NumpadSubtract";
    case KvmScancode::KP_PLUS:
        return "NumpadAdd";
    case KvmScancode::KP_ENTER:
        return "NumpadEnter";
    case KvmScancode::KP_1:
        return "Numpad1";
    case KvmScancode::KP_2:
        return "Numpad2";
    case KvmScancode::KP_3:
        return "Numpad3";
    case KvmScancode::KP_4:
        return "Numpad4";
    case KvmScancode::KP_5:
        return "Numpad5";
    case KvmScancode::KP_6:
        return "Numpad6";
    case KvmScancode::KP_7:
        return "Numpad7";
    case KvmScancode::KP_8:
        return "Numpad8";
    case KvmScancode::KP_9:
        return "Numpad9";
    case KvmScancode::KP_0:
        return "Numpad0";
    case KvmScancode::KP_PERIOD:
        return "NumpadDecimal";
    case KvmScancode::NONUSBACKSLASH:
        return "IntlBackslash";
    case KvmScancode::APPLICATION:
        return "ContextMenu";
    case KvmScancode::KP_EQUALS:
        return "NumpadEqual";

    case KvmScancode::F13:
        return "F13";
    case KvmScancode::F14:
        return "F14";
    case KvmScancode::F15:
        return "F15";
    case KvmScancode::F16:
        return "F16";
    case KvmScancode::F17:
        return "F17";
    case KvmScancode::F18:
        return "F18";
    case KvmScancode::F19:
        return "F19";
    case KvmScancode::F20:
        return "F20";
    case KvmScancode::F21:
        return "F21";
    case KvmScancode::F22:
        return "F22";
    case KvmScancode::F23:
        return "F23";
    case KvmScancode::F24:
        return "F24";

    case KvmScancode::LCTRL:
        return "ControlLeft";
    case KvmScancode::LSHIFT:
        return "ShiftLeft";
    case KvmScancode::LALT:
        return "AltLeft";
    case KvmScancode::LGUI:
        return "MetaLeft";
    case KvmScancode::RCTRL:
        return "ControlRight";
    case KvmScancode::RSHIFT:
        return "ShiftRight";
    case KvmScancode::RALT:
        return "AltRight";
    case KvmScancode::RGUI:
        return "MetaRight";

    default:
        return std::nullopt;
    }
}

std::optional<std::string_view> pikvm_mouse_button_from_button(KvmMouseButton button)
{
    switch (button) {
    case KvmMouseButton::LEFT:
        return "left";
    case KvmMouseButton::MIDDLE:
        return "middle";
    case KvmMouseButton::RIGHT:
        return "right";
    case KvmMouseButton::X1:
        return "up";
    case KvmMouseButton::X2:
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
