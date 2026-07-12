#include "backends/aten/aten_usb_bot.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace hitsc {
namespace {

// CBW field offsets, relative to the start of the 31-byte CBW (standard Bulk-Only Transport).
constexpr std::size_t kCbwTag = 4;                  // dCBWTag
constexpr std::size_t kCbwDataTransferLength = 8;   // dCBWDataTransferLength (LE)
constexpr std::size_t kCbwFlags = 12;               // bmCBWFlags (0x80 = data-IN)
constexpr std::size_t kCbwLun = 13;                 // bCBWLUN
constexpr std::size_t kCbwCbLength = 14;            // bCBWCBLength
constexpr std::size_t kCbwCb = 15;                  // CBWCB (the SCSI CDB)

// A data / CBW / CSW PDU tag as the reference emits it (FillDataPDUTag / FillCSW): byte 0 is
// the literal marker BI_ENP_AND_TK_OUT_DV1 (34), byte 2 the device id, byte 3 a sub-marker
// (0 for a data region, 255 for a CSW region), and bytes 4-7 the region length little-endian.
// This is NOT the big-endian [0,0,0,class] form the small control packets use.
void append_data_pdu_tag(
    std::vector<std::uint8_t>& out, std::uint8_t dev_id, std::uint32_t length, std::uint8_t marker)
{
    out.push_back(static_cast<std::uint8_t>(kAtenClassData));  // 34
    out.push_back(0);
    out.push_back(dev_id);
    out.push_back(marker);
    out.push_back(static_cast<std::uint8_t>(length & 0xFF));
    out.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((length >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((length >> 24) & 0xFF));
}

void append_csw(std::vector<std::uint8_t>& out, const AtenCbw& cbw, std::uint32_t residue, std::uint8_t status)
{
    out.push_back('U');
    out.push_back('S');
    out.push_back('B');
    out.push_back('S');
    out.insert(out.end(), std::begin(cbw.tag), std::end(cbw.tag));
    out.push_back(static_cast<std::uint8_t>(residue & 0xFF));
    out.push_back(static_cast<std::uint8_t>((residue >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((residue >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((residue >> 24) & 0xFF));
    out.push_back(status);
}

// --- canned USB descriptors (verbatim from vstorage.js, for a single CD device) ------------
//
// The reference assembles these in FillUSBPlugInPkt by walking its descriptor tables gated on
// per-interface enable flags. For hitsc's fixed configuration (one device, an ISO CD, no IAD,
// no encryption) only device config descriptor [0] is enabled, so the emitted stream reduces to
// the fixed sequence below. Each block is length-prefixed by its byte count.

// Device descriptor (vuDevRespData[0].resp) with idVendor 0x0B1F / idProduct 0x03EA patched in
// (OFFSET_DEV_DESP_VID=8, OFFSET_DEV_DESP_PID=10).
constexpr std::array<std::uint8_t, 18> kDeviceDescriptor = {
    18, 1, 0, 2, 0, 0, 0, 64, 0x1F, 0x0B, 0xEA, 0x03, 0, 2, 0, 0, 0, 1};

// Configuration descriptor header (vuDevRespData[1].resp[0..8]) with wTotalLength=39 and
// bNumInterfaces=1 already baked (the reference patches these at build time).
constexpr std::array<std::uint8_t, 9> kConfigHeader = {9, 2, 39, 0, 1, 1, 0, 128, 100};

// Interface + 3 endpoint descriptors (st_VSDevConfigDescriptor[0].resp): MSC interface class 8,
// BOT protocol 0x50, bInterfaceNumber=0.
constexpr std::array<std::uint8_t, 30> kInterfaceAndEndpoints = {
    9, 4, 0, 0, 3, 8, 5, 80, 0,
    7, 5, 1, 2, 0, 2, 255,
    7, 5, 130, 2, 0, 2, 255,
    7, 5, 131, 3, 2, 0, 1};

constexpr std::array<std::uint8_t, 4> kStringLangId = {4, 3, 9, 4};
constexpr std::array<std::uint8_t, 34> kStringProduct = {
    34, 3, 70, 0, 108, 0, 97, 0, 115, 0, 104, 0, 32, 0, 68, 0, 105, 0,
    115, 0, 107, 0, 32, 0, 32, 0, 32, 0, 32, 0, 32, 0, 32, 0};
constexpr std::array<std::uint8_t, 34> kStringSerial = {
    34, 3, 52, 0, 69, 0, 56, 0, 70, 0, 48, 0, 57, 0, 50, 0, 67, 0,
    51, 0, 70, 0, 68, 0, 55, 0, 70, 0, 56, 0, 70, 0, 55, 0};
constexpr std::array<std::uint8_t, 26> kStringOem = {
    26, 3, 83, 0, 78, 0, 48, 0, 48, 0, 48, 0, 80, 0, 81, 0, 73, 0, 48, 0, 48, 0, 57, 0, 32, 0};
constexpr std::array<std::uint8_t, 1> kMaxLun = {0};
constexpr std::array<std::uint8_t, 10> kDeviceQualifier = {10, 6, 0, 2, 0, 0, 0, 64, 1, 0};

template <std::size_t N>
void append_length_prefixed(std::vector<std::uint8_t>& out, const std::array<std::uint8_t, N>& block)
{
    out.push_back(static_cast<std::uint8_t>(N));
    out.insert(out.end(), block.begin(), block.end());
}

} // namespace

bool aten_parse_pdu_tag(const std::uint8_t* data, std::size_t size, AtenPduTag& out)
{
    if (data == nullptr || size < kAtenPduTagSize) {
        return false;
    }
    out.pdu_class = (static_cast<std::uint32_t>(data[0]) << 24) |
        (static_cast<std::uint32_t>(data[1]) << 16) |
        (static_cast<std::uint32_t>(data[2]) << 8) |
        static_cast<std::uint32_t>(data[3]);
    out.length = static_cast<std::uint32_t>(data[4]) |
        (static_cast<std::uint32_t>(data[5]) << 8) |
        (static_cast<std::uint32_t>(data[6]) << 16) |
        (static_cast<std::uint32_t>(data[7]) << 24);
    out.dev_id = data[2];
    return true;
}

bool aten_parse_cbw(const std::uint8_t* cbw, std::size_t size, AtenCbw& out)
{
    if (cbw == nullptr || size < kAtenCbwSize) {
        return false;
    }
    std::memcpy(out.tag, cbw + kCbwTag, 4);
    out.data_transfer_length = static_cast<std::uint32_t>(cbw[kCbwDataTransferLength]) |
        (static_cast<std::uint32_t>(cbw[kCbwDataTransferLength + 1]) << 8) |
        (static_cast<std::uint32_t>(cbw[kCbwDataTransferLength + 2]) << 16) |
        (static_cast<std::uint32_t>(cbw[kCbwDataTransferLength + 3]) << 24);
    out.flags = cbw[kCbwFlags];
    out.lun = cbw[kCbwLun];
    out.cb_length = cbw[kCbwCbLength];
    const std::size_t cb_len = std::min<std::size_t>(out.cb_length, sizeof(out.cb));
    std::memcpy(out.cb, cbw + kCbwCb, cb_len);
    return true;
}

std::vector<std::uint8_t> aten_build_data_in_reply(
    const AtenCbw& cbw, std::uint8_t dev_id, std::vector<std::uint8_t> data, std::uint8_t csw_status)
{
    if (data.size() > cbw.data_transfer_length) {
        data.resize(cbw.data_transfer_length);  // never return more than the host asked for
    }
    const std::uint32_t returned = static_cast<std::uint32_t>(data.size());
    const std::uint32_t residue = cbw.data_transfer_length - returned;

    std::vector<std::uint8_t> out;
    out.reserve(kAtenPduTagSize + returned + kAtenPduTagSize + kAtenCswSize);
    append_data_pdu_tag(out, dev_id, returned, /*marker=*/0);
    out.insert(out.end(), data.begin(), data.end());
    append_data_pdu_tag(out, dev_id, kAtenCswSize, /*marker=*/0xFF);  // CSW region
    append_csw(out, cbw, residue, csw_status);
    return out;
}

std::vector<std::uint8_t> aten_build_csw_reply(
    const AtenCbw& cbw, std::uint8_t dev_id, std::uint8_t csw_status, std::uint32_t residue)
{
    std::vector<std::uint8_t> out;
    out.reserve(kAtenPduTagSize + kAtenCswSize);
    append_data_pdu_tag(out, dev_id, kAtenCswSize, /*marker=*/0xFF);
    append_csw(out, cbw, residue, csw_status);
    return out;
}

std::vector<std::uint8_t> aten_build_keepalive_reply()
{
    return {0, 0, 0, 3, 4, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
}

std::vector<std::uint8_t> aten_build_plug_out()
{
    return {0, 0, 0, kAtenClassPlugOut, 0, 0, 0, 0};
}

std::vector<std::uint8_t> aten_build_set_ep()
{
    // FillSetEPCMDPkt for our config: class 7, length 8, payload = [resp[6]=5, count=3, then
    // 3 (endpoint, max-packet) pairs 1/16, 2/32, 3/48]. Length = 2*count+2 = 8.
    return {0, 0, 0, kAtenClassSetEp, 8, 0, 0, 0, 5, 3, 1, 16, 2, 32, 3, 48};
}

std::vector<std::uint8_t> aten_build_plug_in(const AtenPlugInAuth& auth)
{
    std::vector<std::uint8_t> out(52, 0);
    // PDU tag: class 1 (big-endian), length 44 (little-endian) covering the 44-byte auth block.
    out[3] = kAtenClassPlugIn;
    out[4] = 44;

    const std::size_t user_len = std::min<std::size_t>(auth.username.size(), 16);
    std::memcpy(out.data() + 8, auth.username.data(), user_len);
    const std::size_t pass_len = std::min<std::size_t>(auth.password.size(), 20);
    std::memcpy(out.data() + 24, auth.password.data(), pass_len);

    // timestamp nonce, big-endian at bytes 44-47 (the reference writes r>>24, r>>16, r>>8, r).
    out[44] = static_cast<std::uint8_t>((auth.timestamp >> 24) & 0xFF);
    out[45] = static_cast<std::uint8_t>((auth.timestamp >> 16) & 0xFF);
    out[46] = static_cast<std::uint8_t>((auth.timestamp >> 8) & 0xFF);
    out[47] = static_cast<std::uint8_t>(auth.timestamp & 0xFF);

    out[48] = auth.flags;
    out[49] = auth.host_class;
    // out[50], out[51] stay 0.

    // Appended USB descriptors (each length-prefixed), in the exact order FillUSBPlugInPkt emits
    // them for one enabled CD device.
    append_length_prefixed(out, kDeviceDescriptor);  // i=0: device descriptor
    // i=1: config block -- length prefix is the config total (header 9 + interface/endpoints 30).
    out.push_back(static_cast<std::uint8_t>(kConfigHeader.size() + kInterfaceAndEndpoints.size()));
    out.insert(out.end(), kConfigHeader.begin(), kConfigHeader.end());
    out.insert(out.end(), kInterfaceAndEndpoints.begin(), kInterfaceAndEndpoints.end());
    append_length_prefixed(out, kStringLangId);       // i=2
    append_length_prefixed(out, kStringProduct);      // i=3
    append_length_prefixed(out, kStringSerial);       // i=4
    append_length_prefixed(out, kStringOem);          // i=5
    append_length_prefixed(out, kMaxLun);             // i=6
    append_length_prefixed(out, kDeviceQualifier);    // i=7
    return out;
}

AtenCredential aten_split_credential(const std::string& entry_value)
{
    // LENGTH_MAX_USERNAME is 16, so the username is at most the first 15 characters and the
    // password is everything after (StoreVMInfoFromGUI: b_UserNameLength = min(len, 15)).
    AtenCredential credential;
    const std::size_t user_len = std::min<std::size_t>(entry_value.size(), 15);
    credential.username = entry_value.substr(0, user_len);
    credential.password = entry_value.substr(user_len);
    return credential;
}

} // namespace hitsc
