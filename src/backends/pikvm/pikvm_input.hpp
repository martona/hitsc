#pragma once

#include "view_input_types.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace hitsc {

struct PikvmAbsoluteMousePosition {
    int x = 0;
    int y = 0;
};

std::optional<std::string_view> pikvm_key_code_from_scancode(KvmScancode scancode);
std::optional<std::string_view> pikvm_mouse_button_from_button(KvmMouseButton button);

PikvmAbsoluteMousePosition make_pikvm_absolute_mouse_position(double normalized_x, double normalized_y);

std::vector<std::uint8_t> make_pikvm_key_packet(
    std::string_view code,
    bool pressed,
    bool finish = false);

std::vector<std::uint8_t> make_pikvm_mouse_button_packet(
    std::string_view button,
    bool pressed);

std::vector<std::uint8_t> make_pikvm_mouse_move_packet(const PikvmAbsoluteMousePosition& position);

std::vector<std::uint8_t> make_pikvm_mouse_wheel_packet(
    int delta_x,
    int delta_y,
    bool squash = false);

} // namespace hitsc
