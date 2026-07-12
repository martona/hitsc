#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hitsc {

// ATEN (Supermicro) virtual-media "USB-over-IP Bulk-Only Transport" framing for the /vm
// websocket. Unlike MegaRAC's IUSB, the client emulates a WHOLE USB mass-storage device:
// it ships canned USB descriptors in a plug-in packet, the BMC replays enumeration to the
// host, and the host's SCSI then arrives CBW-wrapped over a bulk endpoint. Reverse-engineered
// from the H5Viewer bundle (novnc/js/vstorage.js). Pure byte-wrangling, no networking.
//
// Every message begins with an 8-byte PDU tag: a class dword (bytes 0-3, BIG-endian) and a
// length dword (bytes 4-7, LITTLE-endian); byte 2 doubles as the device id. A CBW arrives as
// a PDU whose length == 31 (a standard Bulk-Only CBW); the reply is a data PDU + a CSW PDU.

inline constexpr std::size_t kAtenPduTagSize = 8;
inline constexpr std::size_t kAtenCbwSize = 31;   // standard Bulk-Only Transport CBW
inline constexpr std::size_t kAtenCswSize = 13;   // "USBS" + tag(4) + residue(4) + status(1)

// PDU classes (the big-endian dword at bytes 0-3).
enum : std::uint32_t {
    kAtenClassData = 34,           // BI_ENP_AND_TK_OUT_DV1: a data / CBW / CSW region
    kAtenClassPlugIn = 1,          // client -> FW: emulate + attach the USB device
    kAtenClassMountStatus = 2,     // FW -> client: plug-in result (COMP_* at byte 8)
    kAtenClassKeepAliveReply = 3,  // client -> FW
    kAtenClassKeepAliveCmd = 4,    // FW -> client
    kAtenClassPlugOut = 5,         // client -> FW: detach the device
    kAtenClassUnmountResp = 6,     // FW -> client: detach acknowledged
    kAtenClassSetEp = 7,           // client -> FW: endpoint max-packet table
};

// Plug-in result codes (the byte at offset 8 of a class-2 mount-status PDU).
enum : std::uint8_t {
    kAtenPlugInOk = 0,            // any value not below is success (the reference's default:)
    kAtenPlugInAuthFail = 1,
    kAtenPlugInBusy = 2,
    kAtenPlugInPrivilege = 3,
    kAtenPlugInDetach = 4,
    kAtenPlugInFwUpdate = 5,
    kAtenPlugInExpire = 6,
};

// host_class values (plug-in byte 49) -- the emulated device category.
enum : std::uint8_t {
    kAtenHostClassFloppy = 1,
    kAtenHostClassHardDisk = 2,
    kAtenHostClassCdrom = 3,  // ISO / CD-ROM
    kAtenHostClassWebIso = 4,
};

struct AtenPduTag {
    std::uint32_t pdu_class = 0;
    std::uint32_t length = 0;
    std::uint8_t dev_id = 0;
};

// Parse the 8-byte PDU tag. `data` must hold >= 8 bytes; returns false otherwise.
bool aten_parse_pdu_tag(const std::uint8_t* data, std::size_t size, AtenPduTag& out);

struct AtenCbw {
    std::uint8_t tag[4] = {0, 0, 0, 0};
    std::uint32_t data_transfer_length = 0;  // dCBWDataTransferLength (LE @8)
    std::uint8_t flags = 0;                   // bmCBWFlags @12 (0x80 = data-IN)
    std::uint8_t lun = 0;
    std::uint8_t cb_length = 0;               // bCBWCBLength @14
    std::uint8_t cb[16] = {0};                // CBWCB (the SCSI CDB) @15

    bool data_in() const { return (flags & 0x80) != 0; }
};

// Parse a 31-byte CBW (the region after the 8-byte PDU tag). `cbw` must hold >= 31 bytes.
bool aten_parse_cbw(const std::uint8_t* cbw, std::size_t size, AtenCbw& out);

// Build a data-IN reply: [8B data PDU tag][data][8B CSW PDU tag][13B CSW]. `data` is clamped
// to the CBW's dCBWDataTransferLength; the CSW residue is (requested - returned). Used whenever
// the CBW's data direction is IN (even for a zero-length data phase).
std::vector<std::uint8_t> aten_build_data_in_reply(
    const AtenCbw& cbw, std::uint8_t dev_id, std::vector<std::uint8_t> data, std::uint8_t csw_status);

// Build a CSW-only reply: [8B CSW PDU tag][13B CSW]. Used for zero-length / data-OUT commands.
std::vector<std::uint8_t> aten_build_csw_reply(
    const AtenCbw& cbw, std::uint8_t dev_id, std::uint8_t csw_status, std::uint32_t residue = 0);

// Fixed control packets.
std::vector<std::uint8_t> aten_build_keepalive_reply();  // [0,0,0,3, 4,0,0,0, FF FF FF FF]
std::vector<std::uint8_t> aten_build_plug_out();          // [0,0,0,5, 0,0,0,0]
std::vector<std::uint8_t> aten_build_set_ep();            // endpoint max-packet table (class 7)

struct AtenPlugInAuth {
    std::string username;             // up to 16 bytes, zero-padded
    std::string password;             // up to 20 bytes, zero-padded
    std::uint32_t timestamp = 0;      // a nonce; the reference uses Math.random() * 32768, BE @44
    std::uint8_t host_class = kAtenHostClassCdrom;
    // totalDev(1) | SESSION_AUTH_ENABLE(0x80) | SESSION_HTML5_VM(0x40) | (dev_idx+1)<<1.
    // 0xC3 for a single device at index 0, which is all hitsc mounts.
    std::uint8_t flags = 0xC3;
};

// Build the plug-in packet: a 52-byte header (class 1, auth, flags, host_class) followed by the
// canned USB descriptors the BMC replays to the host during enumeration. Emulates a single
// removable mass-storage device (VID 0x0B1F / PID 0x03EA "Flash Disk", MSC/BOT).
std::vector<std::uint8_t> aten_build_plug_in(const AtenPlugInAuth& auth);

// The plug-in credential: the bootstrap entry_value split into username (first <=15 chars) and
// password (the remainder), matching the H5Viewer's StoreVMInfoFromGUI.
struct AtenCredential {
    std::string username;
    std::string password;
};
AtenCredential aten_split_credential(const std::string& entry_value);

} // namespace hitsc
