#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace hitsc {

constexpr int kMegaracRelativeMouseMode = 1;
constexpr int kMegaracAbsoluteMouseMode = 2;
constexpr int kMegaracOtherMouseMode = 3;

constexpr std::uint8_t kMegaracKeyboardLeftCtrl = 0x01;
constexpr std::uint8_t kMegaracKeyboardLeftShift = 0x02;
constexpr std::uint8_t kMegaracKeyboardLeftAlt = 0x04;
constexpr std::uint8_t kMegaracKeyboardLeftGui = 0x08;
constexpr std::uint8_t kMegaracKeyboardRightCtrl = 0x10;
constexpr std::uint8_t kMegaracKeyboardRightShift = 0x20;
constexpr std::uint8_t kMegaracKeyboardRightAlt = 0x40;
constexpr std::uint8_t kMegaracKeyboardRightGui = 0x80;

constexpr std::uint8_t kMegaracMouseLeftButton = 1;
constexpr std::uint8_t kMegaracMouseRightButton = 2;
constexpr std::uint8_t kMegaracMouseMiddleButton = 4;
constexpr std::size_t kMegaracKeyboardKeySlots = 6;

using MegaracKeyboardKeySlots = std::array<std::uint8_t, kMegaracKeyboardKeySlots>;

struct MegaracKeyboardReport {
    std::uint8_t modifiers = 0;
    MegaracKeyboardKeySlots keys{};
};

struct MegaracAbsoluteMouseReport {
    std::uint8_t buttons = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int wheel = 0;
};

struct MegaracRelativeMouseReport {
    std::uint8_t buttons = 0;
    int dx = 0;
    int dy = 0;
    int wheel = 0;
};

std::vector<std::uint8_t> make_megarac_absolute_mouse_packet(
    const MegaracAbsoluteMouseReport& report,
    std::uint32_t sequence);
std::vector<std::uint8_t> make_megarac_relative_mouse_packet(
    const MegaracRelativeMouseReport& report,
    std::uint32_t sequence);
std::vector<std::uint8_t> make_megarac_keyboard_packet(
    const MegaracKeyboardReport& report,
    std::uint32_t sequence);

} // namespace hitsc
