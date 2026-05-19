#pragma once

#include "aspeed_decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hitsc {

struct AtenRfbServerInit {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t bits_per_pixel = 0;
    std::uint8_t depth = 0;
    bool big_endian = false;
    bool true_color = false;
    std::uint16_t red_max = 0;
    std::uint16_t green_max = 0;
    std::uint16_t blue_max = 0;
    std::uint8_t red_shift = 0;
    std::uint8_t green_shift = 0;
    std::uint8_t blue_shift = 0;
    std::string name;
    bool insyde_extension = false;
    std::uint32_t session_id = 0;
    std::uint8_t video_enable = 0;
    std::uint8_t keyboard_mouse_enable = 0;
    std::uint8_t kick_user_enable = 0;
    std::uint8_t virtual_media_enable = 0;
};

struct AtenFramebufferRect {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::int32_t encoding = 0;
    std::int32_t mode = 0;
    std::uint32_t data_length = 0;
};

struct AtenAstPayloadHeader {
    unsigned y_selector = 0;
    unsigned uv_selector = 0;
    unsigned mode = 0;
    unsigned mode420 = 0;
};

struct AtenFramebufferUpdateRect {
    AtenFramebufferRect rect;
    std::vector<std::uint8_t> payload;
};

struct AtenCursorPositionMessage {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t valid = 0;
    std::uint32_t pattern_type = 0;
    std::uint64_t pattern_size = 0;
};

enum class AtenRfbMessageKind {
    framebuffer_update,
    bell,
    cursor_position,
    message22,
    mouse_control,
    control_message,
    service,
};

struct AtenRfbMessage {
    AtenRfbMessageKind kind = AtenRfbMessageKind::bell;
    std::uint8_t type = 0;
    std::vector<AtenFramebufferUpdateRect> rects;
    AtenCursorPositionMessage cursor;
    std::uint8_t value = 0;
    std::uint8_t mouse_crypto = 0;
    std::uint8_t mouse_mode = 0;
    std::uint8_t mouse_status = 0;
    std::uint32_t control_count = 0;
    std::uint32_t control_code_digits = 0;
    std::vector<std::uint8_t> control_message;
};

class AtenRfbMessageBuffer {
public:
    void append(const std::vector<std::uint8_t>& bytes);
    void append(std::vector<std::uint8_t>&& bytes);
    void clear();
    std::size_t buffered_bytes() const;
    std::optional<AtenRfbMessage> next();

private:
    bool can_read(std::size_t relative_offset, std::size_t count) const;
    std::uint8_t peek_u8(std::size_t relative_offset) const;
    std::uint16_t peek_be16(std::size_t relative_offset) const;
    std::uint32_t peek_be32(std::size_t relative_offset) const;
    AtenFramebufferRect peek_rect(std::size_t relative_offset) const;
    std::vector<std::uint8_t> slice(std::size_t relative_offset, std::size_t count) const;
    void consume(std::size_t count);
    void compact_if_needed();

    std::vector<std::uint8_t> buffer_;
    std::size_t offset_ = 0;
};

void append_be16(std::vector<std::uint8_t>& bytes, std::uint16_t value);
void append_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value);
std::uint16_t load_be16(const std::vector<std::uint8_t>& bytes, std::size_t offset);
std::uint32_t load_be32(const std::vector<std::uint8_t>& bytes, std::size_t offset);
std::int32_t load_be32_signed(const std::vector<std::uint8_t>& bytes, std::size_t offset);
std::uint32_t load_aten_le32(const std::vector<std::uint8_t>& bytes, std::size_t offset);

std::string client_protocol_version(std::string_view server_version);
bool version_uses_security_types(std::string_view client_version);
bool version_uses_security_result_for_none(std::string_view client_version);
bool is_supported_js_security_type(std::uint8_t type);

std::string security_type_name(std::uint8_t type);
std::string encoding_name(std::int32_t encoding);
std::string aten_server_message_name(std::uint8_t type);
std::string aten_client_packet_name(std::uint8_t type);
std::string describe_aten_client_packet(const std::vector<std::uint8_t>& packet);
std::string hex_preview(const std::vector<std::uint8_t>& bytes, std::size_t limit = 16);

std::vector<std::uint8_t> make_framebuffer_update_request(
    std::uint16_t width,
    std::uint16_t height,
    bool incremental);
std::vector<std::uint8_t> make_aten_key_event(std::uint32_t usage, bool down);
std::vector<std::uint8_t> make_aten_pointer_event(int x, int y, std::uint8_t mask);
std::vector<std::uint8_t> make_aten_cursor_position_request();
std::vector<std::uint8_t> make_aten_mouse_sync_request();
bool is_coalescible_aten_mouse_motion(const std::vector<std::uint8_t>& packet);

AtenAstPayloadHeader read_ast_payload_header(const std::vector<std::uint8_t>& payload);
bool ast_payload_is_frame_end_only(const std::vector<std::uint8_t>& payload);
AspeedDecodeOptions make_aten_aspeed_decode_options(
    int width,
    int height,
    const AtenAstPayloadHeader& ast);

} // namespace hitsc
