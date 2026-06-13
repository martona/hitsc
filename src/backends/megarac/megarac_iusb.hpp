#pragma once

#include "virtual_media/scsi_cd_target.hpp"  // ScsiResult (response builder param)

#include <cstdint>
#include <string>
#include <vector>

namespace hitsc {

// AMI MegaRAC "IUSB" media-redirection framing for the /cd-server websocket. Every packet is
// a 32-byte IUSB header followed by a body; for SCSI the body is an 8-byte sub-header
// (readLength/tagNo/dataDir) then the SCSI command region at offset 41. Reverse-engineered
// from the H5Viewer; see the virtual-media design notes. Pure byte-wrangling, no networking.

// 32-byte IUSB header. dataPacketLength and sequenceNo are little-endian on the wire; the
// signature is the 8 bytes "IUSB    " (four trailing spaces).
struct IusbHeader {
    std::uint8_t major = 1;
    std::uint8_t minor = 0;
    std::uint8_t header_length = 32;
    std::uint32_t data_packet_length = 0;  // number of bytes after the 32-byte header
    std::uint8_t server_caps = 0;
    std::uint8_t device_type = 0;
    std::uint8_t protocol = 0;
    std::uint8_t direction = 0;  // 128 = from client
    std::uint8_t device_no = 0;
    std::uint8_t interface_no = 0;
    std::uint8_t client_data = 0;
    std::uint8_t instance = 0;
    std::uint32_t sequence_no = 0;
    std::uint8_t key[4] = {0, 0, 0, 0};
};

inline constexpr std::size_t kIusbHeaderSize = 32;
inline constexpr std::size_t kIusbScsiOpcodeIndex = 41;  // SCSI CDB opcode, absolute offset
inline constexpr std::size_t kIusbStatusIndex = 53;      // overall status
inline constexpr std::size_t kIusbSenseKeyIndex = 54;    // then ASC @55, ASCQ @56
inline constexpr std::size_t kIusbDataLengthIndex = 57;  // u32 LE, payload byte count
inline constexpr std::size_t kIusbDataIndex = 61;        // payload start
inline constexpr std::uint8_t kIusbDirectionFromClient = 128;

inline constexpr std::uint32_t kIusbDeviceTypeCd = 5;
inline constexpr std::uint32_t kIusbH5Viewer = 3;  // DEVICE_INFO client-type tag

// Control opcodes (carried at the SCSI-opcode offset, 41).
enum : std::uint8_t {
    kIusbOpAck = 241,
    kIusbOpAuth = 242,
    kIusbOpKeepAlive = 243,
    kIusbOpKillRedir = 246,
    kIusbOpDisconnect = 247,
    kIusbOpDeviceInfo = 248,
};

// --- header primitives ---

// Serialize a header into `out32` (>= 32 bytes), leaving the checksum byte for
// iusb_apply_header_checksum().
void iusb_serialize_header(const IusbHeader& header, std::uint8_t* out32);

// Parse the 32-byte header. Returns false if `size` < 32 or the signature is wrong.
bool iusb_parse_header(const std::uint8_t* data, std::size_t size, IusbHeader& out);

// Read just the dataPacketLength field (offset 12, u32 LE) -- the reassembler uses it to know
// the total packet length (= 32 + this) before the whole body has arrived. `data` must hold
// >= 16 bytes.
std::uint32_t iusb_data_packet_length(const std::uint8_t* data);

// Set the checksum byte (offset 11) so the 32 header bytes sum to 0 mod 256. `buf` must hold
// >= 32 bytes. (MegaRAC validates the header checksum server->client; client->server the
// firmware is lenient, but we keep the 32-byte-header convention consistently.)
void iusb_apply_header_checksum(std::uint8_t* buf);

// --- packet builders (client -> server) ---

// Build the outbound IUSB packet for a serviced SCSI command. `request`/`request_size` is the
// full inbound packet (>= 61 bytes); its header + 8-byte sub-header + command region are
// echoed back, then status/sense/dataLength/payload are overwritten with `result`.
std::vector<std::uint8_t> iusb_build_scsi_response(
    const std::uint8_t* request, std::size_t request_size, const ScsiResult& result);

// AUTH (opcode 242): authenticates the redirection with a KVM `token`; `cd_device_no` is the
// CD instance. Optionally sets the media-boost flag.
std::vector<std::uint8_t> iusb_build_auth(
    const std::string& token, std::uint8_t cd_device_no, bool media_boost);

// DEVICE_INFO (opcode 248): advertises the client (H5VIEWER tag) and the mounted image's
// `filename`.
std::vector<std::uint8_t> iusb_build_device_info(const std::string& filename);

// A bare control packet carrying just `opcode` at offset 41 (KEEPALIVE echo, DISCONNECT).
std::vector<std::uint8_t> iusb_build_control(std::uint8_t opcode);

} // namespace hitsc
