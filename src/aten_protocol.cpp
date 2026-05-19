#include "aten_protocol.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace hitsc {
namespace {

constexpr std::uint32_t kAtenAstFrameEndWord = 0x90000000U;
constexpr std::uint64_t kMaxAtenCursorPatternBytes = 16U * 1024U * 1024U;

std::size_t checked_add(std::size_t lhs, std::size_t rhs)
{
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        throw std::runtime_error("ATEN RFB message length overflow");
    }
    return lhs + rhs;
}

} // namespace

void AtenRfbMessageBuffer::append(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty()) {
        return;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void AtenRfbMessageBuffer::append(std::vector<std::uint8_t>&& bytes)
{
    if (bytes.empty()) {
        return;
    }
    if (buffered_bytes() == 0) {
        buffer_ = std::move(bytes);
        offset_ = 0;
        return;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void AtenRfbMessageBuffer::clear()
{
    buffer_.clear();
    offset_ = 0;
}

std::size_t AtenRfbMessageBuffer::buffered_bytes() const
{
    return offset_ <= buffer_.size() ? buffer_.size() - offset_ : 0;
}

std::optional<AtenRfbMessage> AtenRfbMessageBuffer::next()
{
    if (!can_read(0, 1)) {
        return std::nullopt;
    }

    const std::uint8_t type = peek_u8(0);
    AtenRfbMessage message;
    message.type = type;

    switch (type) {
    case 0: {
        if (!can_read(0, 4)) {
            return std::nullopt;
        }

        const std::uint16_t rect_count = peek_be16(2);
        std::size_t cursor = 4;
        for (std::uint16_t i = 0; i < rect_count; ++i) {
            if (!can_read(cursor, 20)) {
                return std::nullopt;
            }
            const std::uint32_t data_length = peek_be32(cursor + 16);
            cursor = checked_add(cursor, 20);
            if (!can_read(cursor, data_length)) {
                return std::nullopt;
            }
            cursor = checked_add(cursor, data_length);
        }

        message.kind = AtenRfbMessageKind::framebuffer_update;
        message.rects.reserve(rect_count);
        std::size_t offset = 4;
        for (std::uint16_t i = 0; i < rect_count; ++i) {
            AtenFramebufferUpdateRect update_rect;
            update_rect.rect = peek_rect(offset);
            offset += 20;
            if (update_rect.rect.data_length != 0) {
                update_rect.payload = slice(offset, update_rect.rect.data_length);
            }
            offset += update_rect.rect.data_length;
            message.rects.push_back(std::move(update_rect));
        }
        consume(cursor);
        return message;
    }
    case 2:
        message.kind = AtenRfbMessageKind::bell;
        consume(1);
        return message;
    case 4: {
        if (!can_read(0, 21)) {
            return std::nullopt;
        }
        const std::uint32_t width = peek_be32(9);
        const std::uint32_t height = peek_be32(13);
        const std::uint32_t valid = peek_be32(17);
        std::uint64_t pattern_size = 0;
        std::size_t total_size = 21;
        if (valid == 1) {
            pattern_size = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 2U;
            if (pattern_size > kMaxAtenCursorPatternBytes) {
                throw std::runtime_error("ATEN RFB cursor pattern is implausibly large");
            }
            if (pattern_size > std::numeric_limits<std::size_t>::max() - 25U) {
                throw std::runtime_error("ATEN RFB cursor pattern length overflow");
            }
            total_size = 25U + static_cast<std::size_t>(pattern_size);
            if (!can_read(0, total_size)) {
                return std::nullopt;
            }
        }

        message.kind = AtenRfbMessageKind::cursor_position;
        message.cursor.x = peek_be32(1);
        message.cursor.y = peek_be32(5);
        message.cursor.width = width;
        message.cursor.height = height;
        message.cursor.valid = valid;
        message.cursor.pattern_size = pattern_size;
        if (valid == 1) {
            message.cursor.pattern_type = peek_be32(21);
        }
        consume(total_size);
        return message;
    }
    case 22:
        if (!can_read(0, 2)) {
            return std::nullopt;
        }
        message.kind = AtenRfbMessageKind::message22;
        message.value = peek_u8(1);
        consume(2);
        return message;
    case 53:
    case 54:
    case 55:
        if (!can_read(0, 4)) {
            return std::nullopt;
        }
        message.kind = AtenRfbMessageKind::mouse_control;
        message.mouse_crypto = peek_u8(1);
        message.mouse_mode = peek_u8(2);
        message.mouse_status = peek_u8(3);
        consume(4);
        return message;
    case 57:
        if (!can_read(0, 265)) {
            return std::nullopt;
        }
        message.kind = AtenRfbMessageKind::control_message;
        message.control_count = peek_be32(1);
        message.control_code_digits = peek_be32(5);
        message.control_message = slice(9, 256);
        consume(265);
        return message;
    case 60:
    case 63:
        if (!can_read(0, 2)) {
            return std::nullopt;
        }
        message.kind = AtenRfbMessageKind::service;
        message.value = peek_u8(1);
        consume(2);
        return message;
    default:
        throw std::runtime_error(
            "unhandled ATEN RFB server message type " + std::to_string(type));
    }
}

bool AtenRfbMessageBuffer::can_read(std::size_t relative_offset, std::size_t count) const
{
    return relative_offset <= buffered_bytes() && count <= buffered_bytes() - relative_offset;
}

std::uint8_t AtenRfbMessageBuffer::peek_u8(std::size_t relative_offset) const
{
    return buffer_.at(offset_ + relative_offset);
}

std::uint16_t AtenRfbMessageBuffer::peek_be16(std::size_t relative_offset) const
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(peek_u8(relative_offset)) << 8)
        | static_cast<std::uint16_t>(peek_u8(relative_offset + 1)));
}

std::uint32_t AtenRfbMessageBuffer::peek_be32(std::size_t relative_offset) const
{
    return (static_cast<std::uint32_t>(peek_u8(relative_offset)) << 24)
        | (static_cast<std::uint32_t>(peek_u8(relative_offset + 1)) << 16)
        | (static_cast<std::uint32_t>(peek_u8(relative_offset + 2)) << 8)
        | static_cast<std::uint32_t>(peek_u8(relative_offset + 3));
}

AtenFramebufferRect AtenRfbMessageBuffer::peek_rect(std::size_t relative_offset) const
{
    AtenFramebufferRect rect;
    rect.x = peek_be16(relative_offset);
    rect.y = peek_be16(relative_offset + 2);
    rect.width = peek_be16(relative_offset + 4);
    rect.height = peek_be16(relative_offset + 6);
    rect.encoding = static_cast<std::int32_t>(peek_be32(relative_offset + 8));
    rect.mode = static_cast<std::int32_t>(peek_be32(relative_offset + 12));
    rect.data_length = peek_be32(relative_offset + 16);
    return rect;
}

std::vector<std::uint8_t> AtenRfbMessageBuffer::slice(std::size_t relative_offset, std::size_t count) const
{
    const auto first = buffer_.begin() + static_cast<std::ptrdiff_t>(offset_ + relative_offset);
    return std::vector<std::uint8_t>(first, first + static_cast<std::ptrdiff_t>(count));
}

void AtenRfbMessageBuffer::consume(std::size_t count)
{
    offset_ += count;
    compact_if_needed();
}

void AtenRfbMessageBuffer::compact_if_needed()
{
    if (offset_ == 0) {
        return;
    }
    if (offset_ == buffer_.size()) {
        clear();
        return;
    }
    if (offset_ >= 4096 && offset_ * 2 >= buffer_.size()) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ = 0;
    }
}

void append_be16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
}

void append_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
}

std::uint16_t load_be16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes.at(offset)) << 8)
        | static_cast<std::uint16_t>(bytes.at(offset + 1)));
}

std::uint32_t load_be32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes.at(offset)) << 24)
        | (static_cast<std::uint32_t>(bytes.at(offset + 1)) << 16)
        | (static_cast<std::uint32_t>(bytes.at(offset + 2)) << 8)
        | static_cast<std::uint32_t>(bytes.at(offset + 3));
}

std::int32_t load_be32_signed(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::int32_t>(load_be32(bytes, offset));
}

std::uint32_t load_aten_le32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes.at(offset))
        | (static_cast<std::uint32_t>(bytes.at(offset + 1)) << 8)
        | (static_cast<std::uint32_t>(bytes.at(offset + 2)) << 16)
        | (static_cast<std::uint32_t>(bytes.at(offset + 3)) << 24);
}

std::string client_protocol_version(std::string_view server_version)
{
    if (server_version == "055.008") {
        return "055.008";
    }
    if (server_version == "004.000" || server_version == "004.001") {
        return "003.008";
    }
    if (server_version == "003.008") {
        return "003.008";
    }
    if (server_version == "003.007") {
        return "003.007";
    }
    if (server_version == "003.003" || server_version == "003.006" || server_version == "003.889") {
        return "003.003";
    }
    throw std::runtime_error("unsupported ATEN RFB protocol version: " + std::string(server_version));
}

bool version_uses_security_types(std::string_view client_version)
{
    return client_version != "003.003";
}

bool version_uses_security_result_for_none(std::string_view client_version)
{
    return client_version == "003.008" || client_version == "055.008";
}

bool is_supported_js_security_type(std::uint8_t type)
{
    return type <= 16 || type == 22 || type == 15;
}

std::string security_type_name(std::uint8_t type)
{
    switch (type) {
    case 1:
        return "None";
    case 2:
        return "VNCAuth";
    case 14:
        return "Tight";
    case 15:
    case 16:
        return "Insyde";
    case 22:
        return "XVP";
    default:
        return "unknown";
    }
}

std::string encoding_name(std::int32_t encoding)
{
    switch (encoding) {
    case 0:
        return "RAW";
    case 1:
        return "COPYRECT";
    case 2:
        return "RRE";
    case 5:
        return "HEXTILE";
    case 7:
        return "TIGHT";
    case 87:
        return "AST2100";
    case 88:
        return "AST2100_JPEG";
    case 89:
        return "RAW_NUVOTON";
    case -223:
        return "DesktopSize";
    case -224:
        return "last_rect";
    case -239:
        return "Cursor";
    case -260:
        return "TIGHT_PNG";
    case -309:
        return "xvp";
    default:
        return "unknown";
    }
}

std::string aten_server_message_name(std::uint8_t type)
{
    switch (type) {
    case 0:
        return "FRAMEBUFFER_UPDATE";
    case 2:
        return "BELL";
    case 4:
        return "INSYDE_CURSOR_POSITION";
    case 22:
        return "INSYDE_MESSAGE_22";
    case 53:
        return "INSYDE_MOUSE_MODE_53";
    case 54:
        return "INSYDE_MOUSE_MODE_54";
    case 55:
        return "INSYDE_MOUSE_MODE_55";
    case 57:
        return "INSYDE_CONTROL_MESSAGE";
    case 60:
        return "INSYDE_SERVICE_60";
    case 63:
        return "INSYDE_SERVICE_63";
    default:
        return "unknown";
    }
}

std::string aten_client_packet_name(std::uint8_t type)
{
    switch (type) {
    case 3:
        return "FRAMEBUFFER_UPDATE_REQUEST";
    case 4:
        return "KEY_EVENT";
    case 5:
        return "POINTER_EVENT";
    case 7:
        return "MOUSE_SYNC";
    case 25:
        return "CURSOR_POSITION_REQUEST";
    case 54:
        return "SET_MOUSE_MODE";
    case 55:
        return "GET_MOUSE_MODE";
    case 63:
        return "VM_SERVICE";
    default:
        return "unknown";
    }
}

std::string describe_aten_client_packet(const std::vector<std::uint8_t>& packet)
{
    if (packet.empty()) {
        return "empty";
    }

    std::ostringstream description;
    description << "type=" << static_cast<int>(packet.front())
                << " " << aten_client_packet_name(packet.front())
                << " bytes=" << packet.size();
    if (packet.front() == 3 && packet.size() >= 10) {
        description << " incremental=" << static_cast<int>(packet[1])
                    << " xy=" << load_be16(packet, 2) << ',' << load_be16(packet, 4)
                    << " size=" << load_be16(packet, 6) << 'x' << load_be16(packet, 8);
    } else if (packet.front() == 4 && packet.size() >= 9) {
        description << " down=" << static_cast<int>(packet[2])
                    << " usage=" << load_be32(packet, 5);
    } else if (packet.front() == 5 && packet.size() >= 7) {
        description << " buttons=" << static_cast<int>(packet[2])
                    << " xy=" << load_be16(packet, 3) << ',' << load_be16(packet, 5);
    }
    return description.str();
}

std::string hex_preview(const std::vector<std::uint8_t>& bytes, std::size_t limit)
{
    std::ostringstream output;
    const std::size_t preview = std::min(bytes.size(), limit);
    for (std::size_t i = 0; i < preview; ++i) {
        if (i != 0) {
            output << ' ';
        }
        output << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(bytes[i])
               << std::dec << std::setfill(' ');
    }
    if (bytes.size() > preview) {
        output << " ...";
    }
    return output.str();
}

std::vector<std::uint8_t> make_framebuffer_update_request(
    std::uint16_t width,
    std::uint16_t height,
    bool incremental)
{
    std::vector<std::uint8_t> request;
    request.push_back(3);
    request.push_back(incremental ? 1 : 0);
    append_be16(request, 0);
    append_be16(request, 0);
    append_be16(request, width);
    append_be16(request, height);
    return request;
}

std::vector<std::uint8_t> make_aten_key_event(std::uint32_t usage, bool down)
{
    std::vector<std::uint8_t> packet;
    packet.reserve(17);
    packet.push_back(4);
    packet.push_back(0);
    packet.push_back(down ? 1U : 0U);
    append_be16(packet, 0);
    append_be32(packet, usage);
    packet.insert(packet.end(), 9, 0);
    return packet;
}

std::vector<std::uint8_t> make_aten_pointer_event(int x, int y, std::uint8_t mask)
{
    std::vector<std::uint8_t> packet;
    packet.reserve(17);
    packet.push_back(5);
    packet.push_back(0);
    packet.push_back(mask);
    append_be16(packet, static_cast<std::uint16_t>(std::clamp(x, 0, 65535)));
    append_be16(packet, static_cast<std::uint16_t>(std::clamp(y, 0, 65535)));
    packet.insert(packet.end(), 11, 0);
    return packet;
}

std::vector<std::uint8_t> make_aten_cursor_position_request()
{
    return std::vector<std::uint8_t>{25};
}

std::vector<std::uint8_t> make_aten_mouse_sync_request()
{
    std::vector<std::uint8_t> packet;
    packet.reserve(3);
    packet.push_back(7);
    append_be16(packet, 1920);
    return packet;
}

bool is_coalescible_aten_mouse_motion(const std::vector<std::uint8_t>& packet)
{
    return packet.size() >= 3 && packet[0] == 5 && packet[2] == 0;
}

AtenAstPayloadHeader read_ast_payload_header(const std::vector<std::uint8_t>& payload)
{
    if (payload.size() < 4) {
        throw std::runtime_error("ATEN AST2100 payload is too short");
    }

    AtenAstPayloadHeader header;
    header.y_selector = payload[0];
    header.uv_selector = payload[1];
    header.mode = (static_cast<unsigned>(payload[2]) << 8) | static_cast<unsigned>(payload[3]);
    header.mode420 = header.mode == 444 ? 0U : 1U;
    return header;
}

bool ast_payload_is_frame_end_only(const std::vector<std::uint8_t>& payload)
{
    return payload.size() >= 8 && load_aten_le32(payload, 4) == kAtenAstFrameEndWord;
}

AspeedDecodeOptions make_aten_aspeed_decode_options(
    int width,
    int height,
    const AtenAstPayloadHeader& ast)
{
    AspeedDecodeOptions decode_options;
    decode_options.width = width;
    decode_options.height = height;
    decode_options.mode420 = ast.mode420;
    decode_options.jpeg_table_selector = ast.y_selector;
    decode_options.chroma_table_selector = ast.uv_selector;
    decode_options.advance_table_selector = 0;
    decode_options.advance_chroma_table_selector = 0;
    decode_options.use_separate_chroma_selectors = true;
    return decode_options;
}

} // namespace hitsc
