#include "virtual_media_tests.hpp"

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

    const std::uint8_t read12[12] = {0xA8, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x00, 0, 0};
    const ScsiCdb b = ScsiCdTarget::parse_cdb(read12);
    expect(b.opcode == 0xA8, "READ(12) opcode parsed");
    expect(b.lba == 0x05, "READ(12) LBA parsed big-endian");
    expect(b.length == 0x0100, "READ(12) length parsed big-endian (u32 @6-9)");
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

    // READ TOC: minimal single-track TOC with a lead-out (0xAA) descriptor.
    const ScsiResult toc = target.execute(ScsiCdb{kScsiReadToc, 0, 0, 20});
    expect(toc.status == 0 && !toc.data.empty() && toc.data.size() <= 20, "READ TOC bounded by allocation");
    expect(toc.data.size() >= 4 && toc.data[2] == 1 && toc.data[3] == 1, "READ TOC reports one track");
    expect(toc.data.size() >= 15 && toc.data[14] == 0xAA, "READ TOC includes the lead-out track");

    // START STOP UNIT is acknowledged; PREVENT/ALLOW MEDIUM REMOVAL is an illegal request.
    expect(target.execute(ScsiCdb{kScsiStartStopUnit, 0, 0, 0}).status == 0, "START STOP UNIT is GOOD");
    const ScsiResult removal = target.execute(ScsiCdb{kScsiMediumRemoval, 0, 0, 0});
    expect(removal.status == 1 && removal.sense_key == 0x05 && removal.asc == 0x20,
           "MEDIUM REMOVAL is ILLEGAL REQUEST 5/20h");
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
    const std::vector<std::uint8_t> auth = iusb_build_auth("abc123", 2, /*media_boost=*/false);
    expect(auth.size() == 193, "AUTH packet is the expected fixed size");
    expect(auth[kIusbScsiOpcodeIndex] == kIusbOpAuth, "AUTH opcode at offset 41");
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

} // namespace

int run_virtual_media_tests()
{
    g_failures = 0;
    test_iso_file_source();
    test_scsi_parse_cdb();
    test_scsi_execute();
    test_iusb_header();
    test_iusb_scsi_response();
    test_iusb_handshake_packets();
    return g_failures;
}

} // namespace hitsc::tests
