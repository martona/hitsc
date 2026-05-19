#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace hitsc {

struct HardwareCursor {
    bool visible = false;
    bool has_pattern = false;
    bool pattern_from_packet = false;
    int type = 0;
    std::uint32_t checksum = 0;
    int x = 0;
    int y = 0;
    int x_offset = 0;
    int y_offset = 0;
    int width = 0;
    int height = 0;
    int pattern_width = 0;
    int pattern_height = 0;
    std::uint64_t sequence = 0;
    std::vector<std::uint16_t> pattern;
};

using MegaracHardwareCursor = HardwareCursor;

struct CursorImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

std::optional<MegaracHardwareCursor> parse_hardware_cursor_packet(
    const std::vector<std::uint8_t>& payload,
    const std::vector<std::uint16_t>& cached_pattern,
    int source_width,
    int source_height);

CursorImage make_cursor_image(
    const HardwareCursor& cursor,
    const std::vector<std::uint8_t>& framebuffer,
    int frame_width,
    int frame_height);

} // namespace hitsc
