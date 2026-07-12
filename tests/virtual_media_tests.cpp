#include "virtual_media_tests.hpp"

#include "backends/aten/aten_usb_bot.hpp"
#include "backends/megarac/megarac_iusb.hpp"
#include "virtual_media/block_source.hpp"
#include "virtual_media/iso_file_source.hpp"
#include "virtual_media/scsi_cd_target.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace hitsc::tests {
namespace {

int g_failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++g_failures;
    }
}

// --- helpers ----------------------------------------------------------------

// Writes `bytes` to a unique temp file and removes it on destruction.
class TempFile {
public:
    explicit TempFile(const std::vector<std::uint8_t>& bytes)
    {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
            ("hitsc_vm_test_" + std::to_string(++counter) + ".iso");
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (!bytes.empty()) {
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
    }

    ~TempFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    std::string string() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// A `sector_count`-sector image whose every byte equals its file offset mod 256, with a valid
// ISO9660 "CD001" descriptor stamped at sector 16 so IsoFileSource accepts it.
std::vector<std::uint8_t> make_fake_iso(std::uint32_t sector_count)
{
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sector_count) * 2048);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(i & 0xFF);
    }
    const std::size_t pvd = 16 * 2048 + 1;
    std::memcpy(bytes.data() + pvd, "CD001", 5);
    return bytes;
}

// A BlockSource that fills each sector with its own (LBA mod 256) byte, so reads are
// position-checkable without touching the disk.
class StubBlockSource : public BlockSource {
public:
    explicit StubBlockSource(std::uint64_t capacity) : capacity_(capacity) {}

    std::uint32_t sector_size() const override { return 2048; }
    std::uint64_t capacity_sectors() const override { return capacity_; }

    bool read(std::uint64_t lba, std::uint32_t count, std::uint8_t* out) override
    {
        for (std::uint32_t s = 0; s < count; ++s) {
            std::memset(out + static_cast<std::size_t>(s) * 2048,
                        static_cast<int>((lba + s) & 0xFF), 2048);
        }
        return true;
    }

private:
    std::uint64_t capacity_;
};

bool header_checksum_ok(const std::uint8_t* buf)
{
    unsigned sum = 0;
    for (std::size_t i = 0; i < kIusbHeaderSize; ++i) {
        sum += buf[i];
    }
    return (sum & 0xFFu) == 0;
}

std::uint32_t le32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// --- IsoFileSource ----------------------------------------------------------

void test_iso_file_source()
{
    const TempFile iso(make_fake_iso(20));  // 20 sectors
    IsoFileSource source(iso.string());

    expect(source.sector_size() == 2048, "ISO sector size is 2048");
    expect(source.capacity_sectors() == 20, "ISO capacity = file size / 2048");

    // In-bounds read returns the file bytes (offset mod 256).
    std::vector<std::uint8_t> buf(2048);
    expect(source.read(17, 1, buf.data()), "ISO in-bounds read succeeds");
    expect(buf[0] == ((17u * 2048u) & 0xFF) && buf[1] == ((17u * 2048u + 1u) & 0xFF) &&
               buf[100] == ((17u * 2048u + 100u) & 0xFF),
           "ISO read returns the file bytes");

    // A read straddling EOF: last real sector (#19) then two past-end sectors -> zeros.
    std::vector<std::uint8_t> straddle(3u * 2048u);
    expect(source.read(19, 3, straddle.data()), "ISO straddling read succeeds");
    expect(straddle[1] == ((19u * 2048u + 1u) & 0xFF), "ISO straddling read keeps real sector");
    bool tail_zero = true;
    for (std::size_t i = 2048; i < straddle.size(); ++i) {
        tail_zero = tail_zero && straddle[i] == 0;
    }
    expect(tail_zero, "ISO read past EOF zero-pads the tail");

    // A read entirely past EOF is all zeros.
    std::vector<std::uint8_t> beyond(2048, 0xCD);
    expect(source.read(100, 1, beyond.data()), "ISO past-EOF read succeeds");
    bool all_zero = true;
    for (const std::uint8_t b : beyond) {
        all_zero = all_zero && b == 0;
    }
    expect(all_zero, "ISO read wholly past EOF is all zeros");

    // A file without an ISO/UDF signature is rejected.
    std::vector<std::uint8_t> junk(20u * 2048u, 0x42);
    const TempFile not_iso(junk);
    bool threw = false;
    try {
        IsoFileSource reject(not_iso.string());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "non-ISO file is rejected at construction");
}

// --- ScsiCdTarget -----------------------------------------------------------

void test_scsi_parse_cdb()
{
    const std::uint8_t read10[12] = {0x28, 0x00, 0x00, 0x00, 0x12, 0x34, 0x00, 0x00, 0x40, 0x00, 0, 0};
    const ScsiCdb a = ScsiCdTarget::parse_cdb(read10);
    expect(a.opcode == 0x28, "READ(10) opcode parsed");
    expect(a.lba == 0x1234, "READ(10) LBA parsed big-endian");
    expect(a.length == 0x40, "READ(10) length parsed big-endian (u16 @7-8)");

    // READ(12) LBA is big-endian (bytes 2-5) but its transfer length is LITTLE-endian (bytes
    // 6-9) -- the AMI BMC convention. Here LBA = 5, length = 2.
    const std::uint8_t read12[12] = {0xA8, 0x00, 0x00, 0x00, 0x00, 0x05, 0x02, 0x00, 0x00, 0x00, 0, 0};
    const ScsiCdb b = ScsiCdTarget::parse_cdb(read12);
    expect(b.opcode == 0xA8, "READ(12) opcode parsed");
    expect(b.lba == 0x05, "READ(12) LBA parsed big-endian");
    expect(b.length == 2, "READ(12) length parsed little-endian (u32 @6-9)");
}

void test_scsi_execute()
{
    StubBlockSource source(1000);
    ScsiCdTarget target(source);

    // First TEST UNIT READY reports a fresh-medium UNIT ATTENTION; the second is GOOD.
    const ScsiResult tur1 = target.execute(ScsiCdb{kScsiTestUnitReady, 0, 0, 0});
    expect(tur1.status == 1 && tur1.sense_key == 0x06 && tur1.asc == 0x28 && tur1.ascq == 0x00,
           "first TUR is UNIT ATTENTION 6/28h/00");
    const ScsiResult tur2 = target.execute(ScsiCdb{kScsiTestUnitReady, 0, 0, 0});
    expect(tur2.status == 0, "second TUR is GOOD");

    // READ CAPACITY: last LBA (999 = 0x3E7) then block length (2048 = 0x800), both big-endian.
    const ScsiResult cap = target.execute(ScsiCdb{kScsiReadCapacity, 0, 0, 0});
    expect(cap.status == 0 && cap.data.size() == 8, "READ CAPACITY returns 8 bytes");
    expect(cap.data[0] == 0x00 && cap.data[1] == 0x00 && cap.data[2] == 0x03 && cap.data[3] == 0xE7,
           "READ CAPACITY last-LBA is big-endian");
    expect(cap.data[4] == 0x00 && cap.data[5] == 0x00 && cap.data[6] == 0x08 && cap.data[7] == 0x00,
           "READ CAPACITY block length is 2048 big-endian");

    // READ(10): 3 sectors from LBA 2 -> each sector filled with its own LBA low byte.
    const ScsiResult read = target.execute(ScsiCdb{kScsiRead10, 0, 2, 3});
    expect(read.status == 0 && read.data.size() == 3u * 2048u, "READ(10) returns count*2048 bytes");
    expect(read.data[0] == 2 && read.data[2048] == 3 && read.data[4096] == 4,
           "READ(10) returns the requested sectors in order");

    // READ TOC: minimal single-track TOC with a lead-out (0xAA) descriptor. The response is
    // always exactly the allocation length -- zero-padded past the 20 real bytes, truncated
    // under -- matching the H5Viewer (the only response shape the BMC has been tested with).
    const ScsiResult toc = target.execute(ScsiCdb{kScsiReadToc, 0, 0, 20});
    expect(toc.status == 0 && toc.data.size() == 20, "READ TOC fills the exact allocation");
    expect(toc.data[0] == 0 && toc.data[1] == 18, "READ TOC header reports the full TOC length");
    expect(toc.data[2] == 1 && toc.data[3] == 1, "READ TOC reports one track");
    expect(toc.data[14] == 0xAA, "READ TOC includes the lead-out track");

    // A larger allocation (the kernel typically asks with a few hundred bytes) is zero-padded;
    // the data-length header still reports only the real 18 bytes that follow it.
    const ScsiResult toc_padded = target.execute(ScsiCdb{kScsiReadToc, 0, 0, 324});
    expect(toc_padded.status == 0 && toc_padded.data.size() == 324, "READ TOC pads to the allocation");
    expect(toc_padded.data[0] == 0 && toc_padded.data[1] == 18 && toc_padded.data[14] == 0xAA,
           "padded READ TOC keeps the real header and descriptors");
    expect(toc_padded.data[20] == 0 && toc_padded.data[323] == 0, "READ TOC pad bytes are zero");

    // A small allocation truncates, and the header reflects the truncated length.
    const ScsiResult toc_short = target.execute(ScsiCdb{kScsiReadToc, 0, 0, 12});
    expect(toc_short.status == 0 && toc_short.data.size() == 12, "READ TOC truncates to the allocation");
    expect(toc_short.data[0] == 0 && toc_short.data[1] == 10,
           "truncated READ TOC header reports the truncated length");

    // An absurd transfer length (e.g. a misparsed READ(12)) is rejected, never allocated.
    const ScsiResult huge = target.execute(ScsiCdb{kScsiRead12, 0, 0, 33554432});
    expect(huge.status == 1 && huge.sense_key == 0x05 && huge.asc == 0x24 && huge.data.empty(),
           "absurd transfer length is ILLEGAL REQUEST, not a giant allocation");

    // START STOP UNIT is acknowledged; PREVENT/ALLOW MEDIUM REMOVAL is an illegal request.
    expect(target.execute(ScsiCdb{kScsiStartStopUnit, 0, 0, 0}).status == 0, "START STOP UNIT is GOOD");
    const ScsiResult removal = target.execute(ScsiCdb{kScsiMediumRemoval, 0, 0, 0});
    expect(removal.status == 1 && removal.sense_key == 0x05 && removal.asc == 0x20,
           "MEDIUM REMOVAL is ILLEGAL REQUEST 5/20h");

    // The BMC-serviced (MegaRAC) persona never answers device-identity commands itself.
    const ScsiResult inq_bmc = target.execute(ScsiCdb{kScsiInquiry, 0, 0, 36});
    expect(inq_bmc.status == 1 && inq_bmc.sense_key == 0x05 && inq_bmc.asc == 0x20,
           "BmcServiced persona rejects INQUIRY (the BMC answers it)");
}

// The ATEN USB-BOT persona emulates the whole USB device, so INQUIRY / REQUEST SENSE / GET
// PERFORMANCE come down to us and must be answered ourselves, and CHECK CONDITIONs are
// retrieved by a following REQUEST SENSE (no autosense).
void test_scsi_full_emulation_persona()
{
    StubBlockSource source(1000);
    ScsiCdTarget target(source, ScsiPersona::FullDeviceEmulation);

    // INQUIRY: 36-byte CD-ROM identity, "ATEN"/"Virtual CDROM".
    ScsiCdb inquiry_cdb{kScsiInquiry, 0, 0, 0};
    inquiry_cdb.length = 36;
    const ScsiResult inq = target.execute(inquiry_cdb);
    expect(inq.status == 0 && inq.data.size() == 36, "INQUIRY returns 36 bytes");
    expect(inq.data[0] == 0x05 && inq.data[1] == 0x80, "INQUIRY reports removable CD-ROM");
    expect(std::memcmp(inq.data.data() + 8, "ATEN", 4) == 0, "INQUIRY vendor is ATEN");
    expect(std::memcmp(inq.data.data() + 16, "Virtual CDROM", 13) == 0, "INQUIRY product is Virtual CDROM");

    // A short allocation length truncates the INQUIRY data.
    ScsiCdb inquiry_short{kScsiInquiry, 0, 0, 0};
    inquiry_short.length = 5;
    expect(target.execute(inquiry_short).data.size() == 5, "INQUIRY honors a short allocation length");

    // GET PERFORMANCE: the canned 20-byte nominal-performance header.
    const ScsiResult perf = target.execute(ScsiCdb{kScsiGetPerformance, 0, 0, 0});
    expect(perf.status == 0 && perf.data.size() == 20, "GET PERFORMANCE returns 20 bytes");
    expect(perf.data[3] == 0x14, "GET PERFORMANCE header length is 0x14");

    // The parse-level allocation-length capture: INQUIRY / REQUEST SENSE take it from CDB byte 4.
    const std::uint8_t inquiry_raw[12] = {0x12, 0x00, 0x00, 0x00, 0x24, 0x00, 0, 0, 0, 0, 0, 0};
    expect(ScsiCdTarget::parse_cdb(inquiry_raw).length == 0x24,
           "INQUIRY allocation length parsed from byte 4");

    // REQUEST SENSE with nothing latched is NO SENSE.
    ScsiCdb sense_cdb{kScsiRequestSense, 0, 0, 0};
    sense_cdb.length = 18;
    const ScsiResult clean = target.execute(sense_cdb);
    expect(clean.status == 0 && clean.data.size() == 18, "REQUEST SENSE returns 18 bytes");
    expect(clean.data[0] == 0x70 && clean.data[2] == 0x00, "REQUEST SENSE with nothing latched is NO SENSE");

    // The first TUR latches UNIT ATTENTION; the following REQUEST SENSE retrieves it, then clears.
    const ScsiResult tur = target.execute(ScsiCdb{kScsiTestUnitReady, 0, 0, 0});
    expect(tur.status == 1 && tur.sense_key == 0x06, "first TUR is UNIT ATTENTION under full emulation");
    const ScsiResult sense = target.execute(sense_cdb);
    expect(sense.data[2] == 0x06 && sense.data[12] == 0x28 && sense.data[13] == 0x00,
           "REQUEST SENSE retrieves the latched UNIT ATTENTION 6/28h/00");
    const ScsiResult sense_again = target.execute(sense_cdb);
    expect(sense_again.data[2] == 0x00 && sense_again.data[12] == 0x00,
           "REQUEST SENSE clears the latch after reporting");

    // A failed read latches MEDIUM ERROR, retrievable by the next REQUEST SENSE.
    ScsiCdb bad_read{kScsiRead10, 0, 0, 0};
    bad_read.length = 100000;  // beyond the 8192-sector cap -> INVALID FIELD IN CDB 5/24h
    const ScsiResult rejected = target.execute(bad_read);
    expect(rejected.status == 1 && rejected.sense_key == 0x05 && rejected.asc == 0x24,
           "over-cap read is ILLEGAL REQUEST under full emulation");
    const ScsiResult after = target.execute(sense_cdb);
    expect(after.data[2] == 0x05 && after.data[12] == 0x24, "REQUEST SENSE retrieves the read rejection");
}

// --- IUSB framing -----------------------------------------------------------

void test_iusb_header()
{
    IusbHeader header;
    header.device_type = kIusbDeviceTypeCd;
    header.direction = kIusbDirectionFromClient;
    header.data_packet_length = 0x1234;
    header.instance = 7;
    header.sequence_no = 0xDEADBEEF;

    std::uint8_t buf[32];
    iusb_serialize_header(header, buf);
    iusb_apply_header_checksum(buf);

    expect(std::memcmp(buf, "IUSB    ", 8) == 0, "IUSB signature serialized");
    expect(header_checksum_ok(buf), "IUSB header bytes sum to 0 mod 256");
    expect(iusb_data_packet_length(buf) == 0x1234, "dataPacketLength read little-endian @12");

    IusbHeader parsed;
    expect(iusb_parse_header(buf, sizeof(buf), parsed), "IUSB header parses back");
    expect(parsed.device_type == kIusbDeviceTypeCd && parsed.direction == kIusbDirectionFromClient &&
               parsed.data_packet_length == 0x1234 && parsed.instance == 7 &&
               parsed.sequence_no == 0xDEADBEEF,
           "IUSB header round-trips its fields");

    std::uint8_t bad[32] = {0};
    expect(!iusb_parse_header(bad, sizeof(bad), parsed), "IUSB parse rejects a bad signature");
}

void test_iusb_scsi_response()
{
    // Craft a plausible inbound READ CAPACITY request: header + sub-header + opcode @41.
    std::vector<std::uint8_t> request(kIusbDataIndex, 0);
    IusbHeader header;
    header.device_type = kIusbDeviceTypeCd;
    iusb_serialize_header(header, request.data());
    request[32] = 0xAA;  // readLength sub-header byte
    request[36] = 0xBB;  // tagNo sub-header byte
    request[40] = 0x01;  // dataDir
    request[kIusbScsiOpcodeIndex] = kScsiReadCapacity;
    iusb_apply_header_checksum(request.data());

    ScsiResult result;
    result.data = {0x00, 0x00, 0x03, 0xE7, 0x00, 0x00, 0x08, 0x00};  // 8-byte capacity payload
    const std::vector<std::uint8_t> out =
        iusb_build_scsi_response(request.data(), request.size(), result);

    expect(out.size() == kIusbDataIndex + 8, "response size = 61 + payload");
    expect(out[19] == kIusbDirectionFromClient, "response direction is from-client");
    expect(out[kIusbScsiOpcodeIndex] == kScsiReadCapacity, "response echoes the request opcode");
    expect(out[32] == 0xAA && out[36] == 0xBB && out[40] == 0x01, "response echoes the sub-header");
    expect(out[kIusbStatusIndex] == 0, "response status is GOOD");
    expect(le32(out.data() + kIusbDataLengthIndex) == 8, "response dataLength is payload size (LE)");
    expect(std::memcmp(out.data() + kIusbDataIndex, result.data.data(), 8) == 0, "response carries the payload");
    expect(iusb_data_packet_length(out.data()) == out.size() - kIusbHeaderSize,
           "response dataPacketLength = total - 32");
    expect(header_checksum_ok(out.data()), "response header checksum is valid");
}

void test_iusb_handshake_packets()
{
    const std::vector<std::uint8_t> auth = iusb_build_auth("abc123", 2);
    expect(auth.size() == 193, "AUTH packet is the expected fixed size");
    expect(auth[kIusbScsiOpcodeIndex] == kIusbOpAuth, "AUTH opcode at offset 41");
    expect(auth[kIusbDataIndex] == 0, "AUTH does not request media-boost (offset 61)");
    expect(auth[23] == 2, "AUTH carries CDDeviceNo in the instance field");
    expect(auth[19] == kIusbDirectionFromClient, "AUTH direction is from-client");
    expect(auth[62] == 0 && std::memcmp(auth.data() + 63, "abc123", 6) == 0, "AUTH token at offset 63");
    expect(iusb_data_packet_length(auth.data()) == auth.size() - kIusbHeaderSize,
           "AUTH dataPacketLength = total - 32");
    expect(header_checksum_ok(auth.data()), "AUTH header checksum is valid");

    const std::vector<std::uint8_t> info = iusb_build_device_info("test.iso");
    expect(info.size() == 291, "DEVICE_INFO packet is the expected fixed size");
    expect(info[kIusbScsiOpcodeIndex] == kIusbOpDeviceInfo, "DEVICE_INFO opcode at offset 41");
    expect(le32(info.data() + 62) == kIusbH5Viewer, "DEVICE_INFO carries the H5VIEWER tag");
    expect(std::memcmp(info.data() + 66, "test.iso", 8) == 0, "DEVICE_INFO carries the filename");
    expect(header_checksum_ok(info.data()), "DEVICE_INFO header checksum is valid");

    const std::vector<std::uint8_t> keepalive = iusb_build_control(kIusbOpKeepAlive);
    expect(keepalive.size() == kIusbDataIndex, "control packet is the framing size");
    expect(keepalive[kIusbScsiOpcodeIndex] == kIusbOpKeepAlive, "control opcode at offset 41");
    expect(header_checksum_ok(keepalive.data()), "control header checksum is valid");
}

// --- ATEN USB-BOT framing ---------------------------------------------------

void test_aten_usb_bot()
{
    // PDU tag round-trip: class is big-endian @0-3, length little-endian @4-7.
    const std::vector<std::uint8_t> plug_out = aten_build_plug_out();
    expect(plug_out.size() == 8, "plug-out is an 8-byte PDU tag");
    AtenPduTag tag{};
    expect(aten_parse_pdu_tag(plug_out.data(), plug_out.size(), tag), "PDU tag parses");
    expect(tag.pdu_class == kAtenClassPlugOut && tag.length == 0, "plug-out class 5, length 0");

    // An incoming CBW arrives wrapped in a data PDU tag [34, 0, devID, 0, len=31 LE]; the
    // transport classifies it by length (31), not class, and reads devID from byte 2.
    std::uint8_t cbw_tag[8] = {34, 0, 3, 0, 31, 0, 0, 0};
    AtenPduTag ct{};
    aten_parse_pdu_tag(cbw_tag, sizeof(cbw_tag), ct);
    expect(ct.length == kAtenCbwSize && ct.dev_id == 3, "CBW PDU tag reports length 31 and devID");

    // CBW parse: a READ(10) of 2 sectors from LBA 5, data-IN.
    std::uint8_t cbw[31] = {0};
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
    cbw[4] = 0xAA; cbw[5] = 0xBB; cbw[6] = 0xCC; cbw[7] = 0xDD;  // tag
    cbw[8] = 0x00; cbw[9] = 0x10;  // dCBWDataTransferLength = 0x1000 = 4096, LE
    cbw[12] = 0x80;  // data-IN
    cbw[14] = 10;    // CB length
    cbw[15] = 0x28; cbw[17] = 0; cbw[18] = 0; cbw[19] = 0; cbw[20] = 5; cbw[22] = 0; cbw[23] = 2;
    AtenCbw parsed{};
    expect(aten_parse_cbw(cbw, sizeof(cbw), parsed), "CBW parses");
    expect(parsed.data_transfer_length == 4096, "CBW dCBWDataTransferLength is little-endian");
    expect(parsed.data_in(), "CBW data direction is IN (flag 0x80)");
    expect(parsed.cb_length == 10 && parsed.cb[0] == 0x28, "CBW carries the SCSI CDB");
    expect(parsed.tag[0] == 0xAA && parsed.tag[3] == 0xDD, "CBW tag captured");

    // Data-IN reply: [8B data tag][data][8B CSW tag][13B CSW]. residue = requested - returned.
    std::vector<std::uint8_t> payload(4096, 0x5A);
    const std::vector<std::uint8_t> reply = aten_build_data_in_reply(parsed, 3, payload, 0);
    expect(reply.size() == 8 + 4096 + 8 + 13, "data-IN reply is data-tag + data + CSW-tag + CSW");
    // Data PDU tag: byte 0 = 34 marker, byte 2 = devID, byte 3 = 0 (data), length LE @4-7.
    expect(reply[0] == 34 && reply[2] == 3 && reply[3] == 0, "data PDU tag: marker 34, devID 3, sub-marker 0");
    const std::uint32_t data_len = reply[4] | (reply[5] << 8) | (reply[6] << 16) |
        (static_cast<std::uint32_t>(reply[7]) << 24);
    expect(data_len == 4096, "data PDU tag reports the payload length little-endian");
    expect(reply[8] == 0x5A, "data payload follows the data tag");
    const std::size_t csw_off = 8 + 4096;
    expect(reply[csw_off] == 34 && reply[csw_off + 3] == 0xFF, "CSW PDU tag: marker 34, sub-marker 255");
    expect(reply[csw_off + 8] == 'U' && reply[csw_off + 9] == 'S' && reply[csw_off + 10] == 'B' &&
           reply[csw_off + 11] == 'S', "CSW starts with the USBS signature");
    expect(reply[csw_off + 12] == 0xAA && reply[csw_off + 15] == 0xDD, "CSW echoes the CBW tag");
    expect(reply[csw_off + 16] == 0 && reply[csw_off + 20] == 0, "full transfer -> residue 0, status GOOD");

    // A short data-IN reply reports a non-zero residue and clamps to the request.
    std::vector<std::uint8_t> short_payload(100, 1);
    const std::vector<std::uint8_t> short_reply = aten_build_data_in_reply(parsed, 3, short_payload, 0);
    const std::size_t short_csw = 8 + 100 + 8;
    const std::uint32_t residue = short_reply[short_csw + 8] |
        (short_reply[short_csw + 9] << 8) | (short_reply[short_csw + 10] << 16) |
        (static_cast<std::uint32_t>(short_reply[short_csw + 11]) << 24);
    expect(residue == 4096 - 100, "short data-IN reply residue = requested - returned");

    // CSW-only reply (zero-length / data-OUT command).
    AtenCbw tur{};
    tur.tag[0] = 0x11;
    const std::vector<std::uint8_t> csw_only = aten_build_csw_reply(tur, 0, 1);
    expect(csw_only.size() == 8 + 13, "CSW-only reply is a CSW PDU tag + CSW");
    expect(csw_only[0] == 34 && csw_only[3] == 0xFF, "CSW-only PDU tag: marker 34, sub-marker 255");
    expect(csw_only[8] == 'U' && csw_only[12] == 0x11 && csw_only[20] == 1,
           "CSW-only carries USBS + tag @12 + fail status @20");

    // Fixed control packets.
    const std::vector<std::uint8_t> keepalive = aten_build_keepalive_reply();
    expect(keepalive.size() == 12 && keepalive[3] == 3 && keepalive[4] == 4 &&
           keepalive[8] == 0xFF && keepalive[11] == 0xFF, "keepalive reply is class 3 + FF FF FF FF");
    const std::vector<std::uint8_t> set_ep = aten_build_set_ep();
    expect(set_ep.size() == 16 && set_ep[3] == kAtenClassSetEp && set_ep[4] == 8,
           "Set-EP is class 7, length 8, 16 bytes");

    // Plug-in packet: 52-byte header + 174 bytes of descriptors = 226.
    AtenPlugInAuth auth;
    auth.username = "admin";
    auth.password = "secretpw";
    auth.timestamp = 0x1234;
    const std::vector<std::uint8_t> plug_in = aten_build_plug_in(auth);
    expect(plug_in.size() == 226, "plug-in packet is 52 header + 174 descriptor bytes");
    expect(plug_in[3] == kAtenClassPlugIn && plug_in[4] == 44, "plug-in class 1, auth length 44");
    expect(std::memcmp(plug_in.data() + 8, "admin", 5) == 0, "plug-in carries the username @8");
    expect(std::memcmp(plug_in.data() + 24, "secretpw", 8) == 0, "plug-in carries the password @24");
    expect(plug_in[46] == 0x12 && plug_in[47] == 0x34, "plug-in timestamp is big-endian @44");
    expect(plug_in[48] == 0xC3, "plug-in flags = totalDev|AUTH|HTML5|(dev+1)<<1");
    expect(plug_in[49] == kAtenHostClassCdrom, "plug-in host_class = 3 (CDROM)");
    expect(plug_in[52] == 18 && plug_in[53] == 18, "first descriptor: length prefix 18, device desc");
    expect(plug_in[52 + 1 + 8] == 0x1F && plug_in[52 + 1 + 9] == 0x0B, "device descriptor idVendor 0x0B1F");
    expect(plug_in[52 + 1 + 10] == 0xEA && plug_in[52 + 1 + 11] == 0x03, "device descriptor idProduct 0x03EA");
    // The config block follows the 19-byte device block; its length prefix is 39.
    expect(plug_in[52 + 19] == 39, "config block length prefix is 39 (header 9 + iface/ep 30)");

    // Credential split: first <=15 chars are the username, the rest the password.
    const AtenCredential short_cred = aten_split_credential("shortcred");
    expect(short_cred.username == "shortcred" && short_cred.password.empty(),
           "credential <=15 chars is all username");
    const AtenCredential long_cred = aten_split_credential("0123456789ABCDEF_password");
    expect(long_cred.username == "0123456789ABCDE" && long_cred.password == "F_password",
           "credential over 15 chars splits at 15");
}

} // namespace

int run_virtual_media_tests()
{
    g_failures = 0;
    test_iso_file_source();
    test_scsi_parse_cdb();
    test_scsi_execute();
    test_scsi_full_emulation_persona();
    test_iusb_header();
    test_iusb_scsi_response();
    test_iusb_handshake_packets();
    test_aten_usb_bot();
    return g_failures;
}

} // namespace hitsc::tests
