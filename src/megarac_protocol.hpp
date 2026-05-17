#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hitsc {

enum class MegaracCommand : std::uint16_t {
    SendHidPacket = 1,
    ResumeRedirection = 6,
    StopSessionImmediate = 8,
    PaintBlankScreen = 9,
    UsbMouseMode = 10,
    GetFullScreen = 11,
    ValidateVideoSession = 18,
    ValidatedVideoSession = 19,
    GetKbdLedStatus = 20,
    GetWebToken = 21,
    MaxSessionClose = 22,
    ConnectionAllowed = 23,
    VideoPackets = 25,
    KvmSharing = 32,
    PowerStatus = 34,
    ActiveClients = 39,
    GetUserMacro = 40,
    SetNextMaster = 50,
    DisplayLockSet = 51,
    DisplayControlStatus = 52,
    MediaLicenseStatus = 53,
    MediaFreeInstanceStatus = 56,
    KeepAlive = 57,
    ConnectionComplete = 58,
    ConnectionFailed = 59,
    FpsDiff = 60,
    KbdQueueStatus = 61,
    IvtpHwCursor = 4098,
    IvtpGetVideoEngineConfigs = 4099,
    IvtpSetVideoEngineConfigs = 4100,
};

constexpr std::uint8_t kMegaracValidateSessionValid = 1;
constexpr std::uint16_t kMegaracViewPrivReqMaster = 1;
constexpr std::uint16_t kMegaracViewReqAllowed = 0;

constexpr std::uint16_t command_value(MegaracCommand command)
{
    return static_cast<std::uint16_t>(command);
}

struct MegaracViewConfig {
    std::string client_ip;
    std::string session;
    std::string token;
    std::string server_ip;
    bool reconnect_enabled = false;
};

struct MegaracPacket {
    std::uint16_t type = 0;
    std::uint32_t payload_size = 0;
    std::uint16_t status = 0;
    std::vector<std::uint8_t> payload;
};

class MegaracPacketBuffer {
public:
    void append(const std::vector<std::uint8_t>& bytes);
    std::optional<MegaracPacket> next();

private:
    void compact_if_needed();

    std::vector<std::uint8_t> buffer_;
    std::size_t offset_ = 0;
};

std::uint16_t load_le16(const std::vector<std::uint8_t>& bytes, std::size_t offset);
std::uint32_t load_le32(const std::vector<std::uint8_t>& bytes, std::size_t offset);

void append_le16(std::vector<std::uint8_t>& bytes, std::uint16_t value);
void append_le32(std::vector<std::uint8_t>& bytes, std::uint32_t value);
void append_fixed_cstring(std::vector<std::uint8_t>& bytes, std::string_view value, std::size_t fixed_size);

std::uint8_t to_byte(int value);
std::string command_name(std::uint16_t type);
std::string max_session_close_reason(std::uint16_t status);
std::string validation_response_name(std::uint8_t response);

std::vector<std::uint8_t> make_header(std::uint16_t type, std::uint32_t payload_size, std::uint16_t status);
std::vector<std::uint8_t> make_simple_packet(std::uint16_t type, std::uint16_t status = 0);
std::vector<std::uint8_t> make_simple_packet(MegaracCommand type, std::uint16_t status = 0);
std::vector<std::uint8_t> make_payload_packet(
    std::uint16_t type,
    std::uint16_t status,
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> make_payload_packet(
    MegaracCommand type,
    std::uint16_t status,
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> make_web_token_packet(std::string_view session);
std::vector<std::uint8_t> make_validate_video_session_packet(
    const MegaracViewConfig& config,
    std::string_view username);

std::uint16_t packet_status_from_bytes(const std::vector<std::uint8_t>& packet);
std::size_t packet_payload_size_from_bytes(const std::vector<std::uint8_t>& packet);
MegaracPacket parse_megarac_packet(const std::vector<std::uint8_t>& bytes);

} // namespace hitsc
