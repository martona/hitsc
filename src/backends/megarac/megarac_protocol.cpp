#include "megarac_protocol.hpp"

#include <algorithm>
#include <stdexcept>

namespace hitsc {
namespace {

constexpr std::size_t kPacketHeaderSize = 8;
constexpr std::size_t kSsiHashSize = 129;
constexpr std::size_t kClientUsernameLength = 129;
constexpr std::size_t kClientOwnIpLength = 65;
constexpr std::size_t kClientOwnMacLength = 49;
constexpr std::size_t kVideoPacketSize = 373;
constexpr std::size_t kWebTokenPayloadLength = 35;
constexpr std::uint32_t kMaxPacketPayloadSize = 128U * 1024U * 1024U;

} // namespace

void MegaracPacketBuffer::append(const std::vector<std::uint8_t>& bytes)
{
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

std::optional<MegaracPacket> MegaracPacketBuffer::next()
{
    if (buffer_.size() - offset_ < kPacketHeaderSize) {
        compact_if_needed();
        return std::nullopt;
    }

    const auto type = load_le16(buffer_, offset_);
    const auto payload_size = load_le32(buffer_, offset_ + 2);
    const auto status = load_le16(buffer_, offset_ + 6);
    if (payload_size > kMaxPacketPayloadSize) {
        throw std::runtime_error("MegaRAC packet declared implausible payload size");
    }

    const std::size_t packet_size = kPacketHeaderSize + payload_size;
    if (buffer_.size() - offset_ < packet_size) {
        compact_if_needed();
        return std::nullopt;
    }

    MegaracPacket packet;
    packet.type = type;
    packet.payload_size = payload_size;
    packet.status = status;
    packet.payload.assign(
        buffer_.begin() + static_cast<std::ptrdiff_t>(offset_ + kPacketHeaderSize),
        buffer_.begin() + static_cast<std::ptrdiff_t>(offset_ + packet_size));
    offset_ += packet_size;
    compact_if_needed();
    return packet;
}

void MegaracPacketBuffer::compact_if_needed()
{
    if (offset_ == 0) {
        return;
    }
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
        return;
    }
    if (offset_ > 4096) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ = 0;
    }
}

std::uint16_t load_le16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    if (offset + 2 > bytes.size()) {
        throw std::runtime_error("truncated little-endian uint16");
    }
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

std::uint32_t load_le32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size()) {
        throw std::runtime_error("truncated little-endian uint32");
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void append_le16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void append_le32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

void append_fixed_cstring(std::vector<std::uint8_t>& bytes, std::string_view value, std::size_t fixed_size)
{
    const std::size_t copy_size = std::min(value.size(), fixed_size);
    for (std::size_t i = 0; i < copy_size; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value[i]));
    }
    bytes.insert(bytes.end(), fixed_size - copy_size, 0);
}

std::uint8_t to_byte(int value)
{
    return static_cast<std::uint8_t>(value & 0xff);
}

std::string command_name(std::uint16_t type)
{
    switch (type) {
    case command_value(MegaracCommand::SendHidPacket):
        return "CMD_SEND_HID_PACKET";
    case 4:
        return "CMD_PAUSE_REDIRECTION";
    case command_value(MegaracCommand::ResumeRedirection):
        return "CMD_RESUME_REDIRECTION";
    case command_value(MegaracCommand::StopSessionImmediate):
        return "CMD_STOP_SESSION_IMMEDIATE";
    case command_value(MegaracCommand::PaintBlankScreen):
        return "CMD_PAINT_BLANK_SCREEN";
    case command_value(MegaracCommand::UsbMouseMode):
        return "CMD_USB_MOUSE_MODE";
    case command_value(MegaracCommand::GetFullScreen):
        return "CMD_GET_FULL_SCREEN";
    case command_value(MegaracCommand::ValidateVideoSession):
        return "CMD_VALIDATE_VIDEO_SESSION";
    case command_value(MegaracCommand::ValidatedVideoSession):
        return "CMD_VALIDATED_VIDEO_SESSION";
    case command_value(MegaracCommand::MaxSessionClose):
        return "CMD_MAX_SESSION_CLOSE";
    case command_value(MegaracCommand::GetKbdLedStatus):
        return "CMD_GET_KBD_LED_STATUS";
    case command_value(MegaracCommand::GetWebToken):
        return "CMD_GET_WEB_TOKEN";
    case command_value(MegaracCommand::ConnectionAllowed):
        return "CMD_CONNECTION_ALLOWED";
    case command_value(MegaracCommand::VideoPackets):
        return "CMD_VIDEO_PACKETS";
    case command_value(MegaracCommand::KvmSharing):
        return "CMD_KVM_SHARING";
    case command_value(MegaracCommand::PowerStatus):
        return "CMD_POWER_STATUS";
    case 37:
        return "CMD_SERVICE_INFO";
    case 38:
        return "CMD_KVM_MEDIA_INFO";
    case command_value(MegaracCommand::ActiveClients):
        return "CMD_ACTIVE_CLIENTS";
    case command_value(MegaracCommand::GetUserMacro):
        return "CMD_GET_USER_MACRO";
    case command_value(MegaracCommand::SetNextMaster):
        return "CMD_SET_NEXT_MASTER";
    case command_value(MegaracCommand::DisplayLockSet):
        return "CMD_DISPLAY_LOCK_SET";
    case command_value(MegaracCommand::DisplayControlStatus):
        return "CMD_DISPLAY_CONTROL_STATUS";
    case command_value(MegaracCommand::MediaLicenseStatus):
        return "CMD_MEDIA_LICENSE_STATUS";
    case command_value(MegaracCommand::MediaFreeInstanceStatus):
        return "CMD_MEDIA_FREE_INSTANCE_STATUS";
    case command_value(MegaracCommand::KeepAlive):
        return "CMD_KEEP_ALIVE_PKT";
    case command_value(MegaracCommand::ConnectionComplete):
        return "CMD_CONNECTION_COMPLETE_PKT";
    case command_value(MegaracCommand::ConnectionFailed):
        return "CMD_CONNECTION_FAILED";
    case command_value(MegaracCommand::FpsDiff):
        return "CMD_FPS_DIFF";
    case command_value(MegaracCommand::KbdQueueStatus):
        return "CMD_KBD_QUEUE_STATUS";
    case command_value(MegaracCommand::IvtpHwCursor):
        return "IVTP_HW_CURSOR";
    case command_value(MegaracCommand::IvtpGetVideoEngineConfigs):
        return "IVTP_GET_VIDEO_ENGINE_CONFIGS";
    case command_value(MegaracCommand::IvtpSetVideoEngineConfigs):
        return "IVTP_SET_VIDEO_ENGINE_CONFIGS";
    default:
        return "UNKNOWN";
    }
}

std::string max_session_close_reason(std::uint16_t status)
{
    switch (status) {
    case 0:
        return "maximum KVM sessions reached";
    case 1:
        return "same KVM client/user already connected";
    default:
        return "unknown session close reason";
    }
}

std::string validation_response_name(std::uint8_t response)
{
    switch (response) {
    case 0:
        return "invalid session";
    case kMegaracValidateSessionValid:
        return "valid session";
    case 2:
        return "not sufficient privilege";
    case 3:
        return "invalid session info";
    case 8:
        return "session unregistered";
    default:
        return "unknown validation response";
    }
}

std::vector<std::uint8_t> make_header(std::uint16_t type, std::uint32_t payload_size, std::uint16_t status)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kPacketHeaderSize + payload_size);
    append_le16(bytes, type);
    append_le32(bytes, payload_size);
    append_le16(bytes, status);
    return bytes;
}

std::vector<std::uint8_t> make_simple_packet(std::uint16_t type, std::uint16_t status)
{
    return make_header(type, 0, status);
}

std::vector<std::uint8_t> make_payload_packet(
    std::uint16_t type,
    std::uint16_t status,
    const std::vector<std::uint8_t>& payload)
{
    std::vector<std::uint8_t> bytes = make_header(
        type,
        static_cast<std::uint32_t>(payload.size()),
        status);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<std::uint8_t> make_simple_packet(MegaracCommand type, std::uint16_t status)
{
    return make_simple_packet(command_value(type), status);
}

std::vector<std::uint8_t> make_payload_packet(
    MegaracCommand type,
    std::uint16_t status,
    const std::vector<std::uint8_t>& payload)
{
    return make_payload_packet(command_value(type), status, payload);
}

std::vector<std::uint8_t> make_web_token_packet(std::string_view session)
{
    std::vector<std::uint8_t> payload;
    payload.reserve(kWebTokenPayloadLength);
    append_fixed_cstring(payload, session, kWebTokenPayloadLength);
    return make_payload_packet(MegaracCommand::GetWebToken, 0, payload);
}

std::vector<std::uint8_t> make_validate_video_session_packet(
    const MegaracViewConfig& config,
    std::string_view username)
{
    std::vector<std::uint8_t> payload;
    payload.reserve(kVideoPacketSize + kClientOwnIpLength);
    payload.push_back(0);
    append_fixed_cstring(payload, config.token, kSsiHashSize);
    append_fixed_cstring(payload, config.client_ip, kClientOwnIpLength);
    append_fixed_cstring(payload, username.empty() ? "domain/username" : username, kClientUsernameLength);
    append_fixed_cstring(payload, "00-00-00-00-00-00", kClientOwnMacLength);
    append_fixed_cstring(payload, config.server_ip, kClientOwnIpLength);

    std::vector<std::uint8_t> bytes;
    if (config.reconnect_enabled) {
        bytes = make_simple_packet(MegaracCommand::ConnectionComplete, 1);
    }

    std::vector<std::uint8_t> validate = make_header(
        command_value(MegaracCommand::ValidateVideoSession),
        static_cast<std::uint32_t>(payload.size()),
        1);
    validate.insert(validate.end(), payload.begin(), payload.end());
    bytes.insert(bytes.end(), validate.begin(), validate.end());
    std::vector<std::uint8_t> resume = make_simple_packet(MegaracCommand::ResumeRedirection);
    bytes.insert(bytes.end(), resume.begin(), resume.end());
    return bytes;
}

std::uint16_t packet_status_from_bytes(const std::vector<std::uint8_t>& packet)
{
    if (packet.size() < kPacketHeaderSize) {
        return 0;
    }
    return static_cast<std::uint16_t>(packet[6]) |
           static_cast<std::uint16_t>(packet[7] << 8);
}

std::size_t packet_payload_size_from_bytes(const std::vector<std::uint8_t>& packet)
{
    return packet.size() >= kPacketHeaderSize ? packet.size() - kPacketHeaderSize : 0;
}

MegaracPacket parse_megarac_packet(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.size() < kPacketHeaderSize) {
        throw std::runtime_error("truncated MegaRAC packet");
    }

    MegaracPacket packet;
    packet.type = load_le16(bytes, 0);
    packet.payload_size = load_le32(bytes, 2);
    packet.status = load_le16(bytes, 6);
    if (packet.payload_size != bytes.size() - kPacketHeaderSize) {
        throw std::runtime_error("MegaRAC packet payload size mismatch");
    }
    packet.payload.assign(bytes.begin() + kPacketHeaderSize, bytes.end());
    return packet;
}

} // namespace hitsc
