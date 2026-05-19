#include "hardware_cursor.hpp"

#include "megarac_protocol.hpp"

#include <algorithm>

namespace hitsc {
namespace {

constexpr std::size_t kHardwareCursorHeaderSize = 13;
constexpr std::size_t kHardwareCursorPatternSize = 64;
constexpr std::size_t kHardwareCursorPatternPixels =
    kHardwareCursorPatternSize * kHardwareCursorPatternSize;
constexpr int kHardwareCursorFallbackWidth = 15;
constexpr int kHardwareCursorFallbackHeight = 15;
constexpr int kInitialFramebufferWidth = 800;
constexpr int kInitialFramebufferHeight = 600;

std::vector<std::uint16_t> parse_hardware_cursor_pattern(const std::vector<std::uint8_t>& payload)
{
    std::vector<std::uint16_t> pattern(kHardwareCursorPatternPixels, 0);
    const std::size_t available_words = (payload.size() - kHardwareCursorHeaderSize) / 2;
    const std::size_t words = std::min(available_words, kHardwareCursorPatternPixels);
    for (std::size_t index = 0; index < words; ++index) {
        pattern[index] = load_le16(payload, kHardwareCursorHeaderSize + index * 2);
    }
    return pattern;
}

void set_cursor_pixel(
    CursorImage& image,
    int x,
    int y,
    std::uint8_t red,
    std::uint8_t green,
    std::uint8_t blue,
    std::uint8_t alpha)
{
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
        return;
    }

    const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 4;
    image.rgba[offset + 0] = red;
    image.rgba[offset + 1] = green;
    image.rgba[offset + 2] = blue;
    image.rgba[offset + 3] = alpha;
}

CursorImage make_fallback_cursor_image(const HardwareCursor& cursor, int frame_width, int frame_height)
{
    CursorImage image;
    image.width = std::min(kHardwareCursorFallbackWidth, std::max(0, frame_width - cursor.x));
    image.height = std::min(kHardwareCursorFallbackHeight, std::max(0, frame_height - cursor.y));
    if (image.width <= 0 || image.height <= 0) {
        return {};
    }

    image.rgba.assign(static_cast<std::size_t>(image.width) * image.height * 4, 0);
    const int center_x = std::min(7, image.width - 1);
    const int center_y = std::min(7, image.height - 1);
    for (int x = 0; x < image.width; ++x) {
        set_cursor_pixel(image, x, center_y, 0, 0, 0, 255);
        set_cursor_pixel(image, x, center_y - 1, 255, 255, 255, 255);
    }
    for (int y = 0; y < image.height; ++y) {
        set_cursor_pixel(image, center_x, y, 0, 0, 0, 255);
        set_cursor_pixel(image, center_x - 1, y, 255, 255, 255, 255);
    }
    return image;
}

} // namespace

std::optional<MegaracHardwareCursor> parse_hardware_cursor_packet(
    const std::vector<std::uint8_t>& payload,
    const std::vector<std::uint16_t>& cached_pattern,
    int source_width,
    int source_height)
{
    if (payload.size() < kHardwareCursorHeaderSize) {
        return std::nullopt;
    }

    if (source_width <= 0) {
        source_width = kInitialFramebufferWidth;
    }
    if (source_height <= 0) {
        source_height = kInitialFramebufferHeight;
    }

    MegaracHardwareCursor cursor;
    cursor.type = payload[0];
    cursor.checksum = load_le32(payload, 1);
    cursor.x = load_le16(payload, 5);
    cursor.y = load_le16(payload, 7);
    cursor.x_offset = std::min<int>(load_le16(payload, 9), static_cast<int>(kHardwareCursorPatternSize - 1));
    cursor.y_offset = std::min<int>(load_le16(payload, 11), static_cast<int>(kHardwareCursorPatternSize - 1));
    cursor.pattern_width = kHardwareCursorPatternSize;
    cursor.pattern_height = kHardwareCursorPatternSize;

    if (payload.size() > kHardwareCursorHeaderSize) {
        cursor.pattern = parse_hardware_cursor_pattern(payload);
        cursor.pattern_from_packet = true;
    } else if (!cached_pattern.empty()) {
        cursor.pattern = cached_pattern;
    }

    cursor.has_pattern = !cursor.pattern.empty();
    if (cursor.x >= source_width || cursor.y >= source_height) {
        cursor.visible = false;
        return cursor;
    }

    cursor.width = std::min(
        source_width - cursor.x,
        std::max(0, cursor.pattern_width - cursor.x_offset));
    if (cursor.type != 1) {
        cursor.width = std::min(cursor.width, 32);
    }
    cursor.height = std::min(
        source_height - cursor.y,
        std::max(0, cursor.pattern_height - cursor.y_offset));
    cursor.visible = cursor.width > 0 && cursor.height > 0;
    return cursor;
}

CursorImage make_cursor_image(
    const HardwareCursor& cursor,
    const std::vector<std::uint8_t>& framebuffer,
    int frame_width,
    int frame_height)
{
    if (!cursor.visible || frame_width <= 0 || frame_height <= 0) {
        return {};
    }
    const int pattern_width = cursor.pattern_width > 0 ? cursor.pattern_width : kHardwareCursorPatternSize;
    const int pattern_height = cursor.pattern_height > 0 ? cursor.pattern_height : kHardwareCursorPatternSize;
    const std::size_t pattern_pixels =
        static_cast<std::size_t>(pattern_width) * static_cast<std::size_t>(pattern_height);
    if (!cursor.has_pattern || cursor.pattern.size() < pattern_pixels) {
        return make_fallback_cursor_image(cursor, frame_width, frame_height);
    }

    CursorImage image;
    image.width = std::min(cursor.width, std::max(0, frame_width - cursor.x));
    image.height = std::min(cursor.height, std::max(0, frame_height - cursor.y));
    if (image.width <= 0 || image.height <= 0) {
        return {};
    }

    image.rgba.assign(static_cast<std::size_t>(image.width) * image.height * 4, 0);
    const bool has_framebuffer =
        framebuffer.size() == static_cast<std::size_t>(frame_width) * frame_height * 4;

    for (int row = 0; row < image.height; ++row) {
        for (int column = 0; column < image.width; ++column) {
            const std::size_t pattern_index =
                static_cast<std::size_t>(row + cursor.y_offset) * static_cast<std::size_t>(pattern_width)
                + static_cast<std::size_t>(column + cursor.x_offset);
            const std::uint16_t cursor_data = cursor.pattern[pattern_index];
            const std::uint8_t red = static_cast<std::uint8_t>((cursor_data & 0x0f00) >> 4);
            const std::uint8_t green = static_cast<std::uint8_t>(cursor_data & 0x00f0);
            const std::uint8_t blue = static_cast<std::uint8_t>((cursor_data & 0x000f) << 4);

            if (cursor.type == 1) {
                const auto alpha_nibble = static_cast<std::uint8_t>((cursor_data & 0xf000) >> 12);
                if (alpha_nibble == 0) {
                    continue;
                }
                set_cursor_pixel(
                    image,
                    column,
                    row,
                    red,
                    green,
                    blue,
                    static_cast<std::uint8_t>(alpha_nibble * 17));
                continue;
            }

            const bool and_bit = (cursor_data & 0x8000) != 0;
            const bool xor_bit = (cursor_data & 0x4000) != 0;
            if (!and_bit) {
                set_cursor_pixel(image, column, row, red, green, blue, 255);
            } else if (xor_bit) {
                if (has_framebuffer) {
                    const std::size_t source =
                        (static_cast<std::size_t>(cursor.y + row) * frame_width + cursor.x + column) * 4;
                    set_cursor_pixel(
                        image,
                        column,
                        row,
                        static_cast<std::uint8_t>(255 - framebuffer[source + 0]),
                        static_cast<std::uint8_t>(255 - framebuffer[source + 1]),
                        static_cast<std::uint8_t>(255 - framebuffer[source + 2]),
                        255);
                } else {
                    set_cursor_pixel(image, column, row, 255, 255, 255, 255);
                }
            }
        }
    }

    return image;
}

} // namespace hitsc
