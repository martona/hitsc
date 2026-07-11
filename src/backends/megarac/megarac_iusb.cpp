#include "backends/megarac/megarac_iusb.hpp"

#include <algorithm>
#include <cstring>

namespace hitsc {
namespace {

const char kIusbSignature[8] = {'I', 'U', 'S', 'B', ' ', ' ', ' ', ' '};

void write_le32(std::uint8_t* p, std::uint32_t value)
{
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
    p[2] = static_cast<std::uint8_t>(value >> 16);
    p[3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint32_t read_le32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// The AUTH and DEVICE_INFO packets are fixed-size with the payload written into a zero-padded
// field, matching the H5Viewer's buffer sizing. (The exact totals reproduce the JS so a
// validating firmware sees the same lengths; confirm against a live /cd-server in Phase 2.)
constexpr std::size_t kAuthTokenOffset = 63;   // a 0 byte sits at 62, token string from 63
constexpr std::size_t kAuthTotalSize = 193;    // 32 hdr + 62 + 98 (WEB_AUTH_PKT_MAX) + 1
constexpr std::size_t kDeviceInfoTagOffset = 62;   // H5VIEWER u32 LE @62, filename from 66
constexpr std::size_t kDeviceInfoNameOffset = 66;
constexpr std::size_t kDeviceInfoTotalSize = 291;  // 32 + (62-32) + 260 (DEVICE_INFO_MAX) + 1

} // namespace

void iusb_serialize_header(const IusbHeader& header, std::uint8_t* out)
{
    std::memcpy(out, kIusbSignature, 8);
    out[8] = header.major;
    out[9] = header.minor;
    out[10] = header.header_length;
    out[11] = 0;  // checksum placeholder; iusb_apply_header_checksum fills it
    write_le32(out + 12, header.data_packet_length);
    out[16] = header.server_caps;
    out[17] = header.device_type;
    out[18] = header.protocol;
    out[19] = header.direction;
    out[20] = header.device_no;
    out[21] = header.interface_no;
    out[22] = header.client_data;
    out[23] = header.instance;
    write_le32(out + 24, header.sequence_no);
    std::memcpy(out + 28, header.key, 4);
}

bool iusb_parse_header(const std::uint8_t* data, std::size_t size, IusbHeader& out)
{
    if (size < kIusbHeaderSize || std::memcmp(data, kIusbSignature, 8) != 0) {
        return false;
    }
    out.major = data[8];
    out.minor = data[9];
    out.header_length = data[10];
    // data[11] is the checksum byte; not retained.
    out.data_packet_length = read_le32(data + 12);
    out.server_caps = data[16];
    out.device_type = data[17];
    out.protocol = data[18];
    out.direction = data[19];
    out.device_no = data[20];
    out.interface_no = data[21];
    out.client_data = data[22];
    out.instance = data[23];
    out.sequence_no = read_le32(data + 24);
    std::memcpy(out.key, data + 28, 4);
    return true;
}

std::uint32_t iusb_data_packet_length(const std::uint8_t* data)
{
    return read_le32(data + 12);
}

void iusb_apply_header_checksum(std::uint8_t* buf)
{
    buf[11] = 0;
    unsigned sum = 0;
    for (std::size_t i = 0; i < kIusbHeaderSize; ++i) {
        sum += buf[i];
    }
    buf[11] = static_cast<std::uint8_t>((0u - sum) & 0xFFu);
}

std::vector<std::uint8_t> iusb_build_scsi_response(
    const std::uint8_t* request, std::size_t request_size, const ScsiResult& result)
{
    const std::size_t total = kIusbDataIndex + result.data.size();  // 61 + payload
    std::vector<std::uint8_t> out(total, 0);

    // Echo the inbound header + 8-byte sub-header + command region (offsets 0..60); the BMC
    // matches the response to its request via the sub-header (readLength/tagNo) and command.
    const std::size_t echo = std::min<std::size_t>(request_size, kIusbDataIndex);
    std::memcpy(out.data(), request, echo);

    // Header fixups: this packet now flows client -> server, and its body length changed.
    out[19] = kIusbDirectionFromClient;
    write_le32(out.data() + 12, static_cast<std::uint32_t>(total - kIusbHeaderSize));

    // Status + sense.
    out[kIusbStatusIndex] = result.status;
    out[kIusbSenseKeyIndex] = result.sense_key;
    out[kIusbSenseKeyIndex + 1] = result.asc;
    out[kIusbSenseKeyIndex + 2] = result.ascq;

    // dataLength (u32 LE) = payload byte count, then the payload itself.
    write_le32(out.data() + kIusbDataLengthIndex, static_cast<std::uint32_t>(result.data.size()));
    if (!result.data.empty()) {
        std::memcpy(out.data() + kIusbDataIndex, result.data.data(), result.data.size());
    }

    iusb_apply_header_checksum(out.data());
    return out;
}

std::vector<std::uint8_t> iusb_build_auth(const std::string& token, std::uint8_t cd_device_no)
{
    std::vector<std::uint8_t> out(kAuthTotalSize, 0);

    IusbHeader header;
    header.direction = kIusbDirectionFromClient;
    header.instance = cd_device_no;  // CDDeviceNo rides in the instance field (offset 23)
    header.data_packet_length = static_cast<std::uint32_t>(kAuthTotalSize - kIusbHeaderSize);
    iusb_serialize_header(header, out.data());

    out[kIusbScsiOpcodeIndex] = kIusbOpAuth;
    const std::size_t room = kAuthTotalSize - kAuthTokenOffset;
    const std::size_t copy = std::min(token.size(), room);
    std::memcpy(out.data() + kAuthTokenOffset, token.data(), copy);

    iusb_apply_header_checksum(out.data());
    return out;
}

std::vector<std::uint8_t> iusb_build_device_info(const std::string& filename)
{
    std::vector<std::uint8_t> out(kDeviceInfoTotalSize, 0);

    IusbHeader header;
    header.direction = kIusbDirectionFromClient;
    header.data_packet_length =
        static_cast<std::uint32_t>(kDeviceInfoTotalSize - kIusbHeaderSize);
    iusb_serialize_header(header, out.data());

    out[kIusbScsiOpcodeIndex] = kIusbOpDeviceInfo;
    write_le32(out.data() + kDeviceInfoTagOffset, kIusbH5Viewer);
    const std::size_t room = kDeviceInfoTotalSize - kDeviceInfoNameOffset - 1;  // keep a NUL
    const std::size_t copy = std::min(filename.size(), room);
    std::memcpy(out.data() + kDeviceInfoNameOffset, filename.data(), copy);

    iusb_apply_header_checksum(out.data());
    return out;
}

std::vector<std::uint8_t> iusb_build_control(std::uint8_t opcode)
{
    // A control packet still carries the full SCSI framing region; only the opcode at 41
    // matters. Size it to the data index so the offsets line up.
    std::vector<std::uint8_t> out(kIusbDataIndex, 0);

    IusbHeader header;
    header.direction = kIusbDirectionFromClient;
    header.data_packet_length = static_cast<std::uint32_t>(kIusbDataIndex - kIusbHeaderSize);
    iusb_serialize_header(header, out.data());

    out[kIusbScsiOpcodeIndex] = opcode;

    iusb_apply_header_checksum(out.data());
    return out;
}

} // namespace hitsc
