#include "virtual_media/scsi_cd_target.hpp"

#include "virtual_media/block_source.hpp"

#include <algorithm>

namespace hitsc {
namespace {

std::uint32_t read_be16(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 8) | static_cast<std::uint32_t>(p[1]);
}

std::uint32_t read_be32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
        (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

void push_be32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

// CHECK CONDITION with the given sense triple.
ScsiResult check_condition(std::uint8_t sense_key, std::uint8_t asc, std::uint8_t ascq)
{
    return ScsiResult{/*status=*/1, sense_key, asc, ascq, {}};
}

} // namespace

ScsiCdb ScsiCdTarget::parse_cdb(const std::uint8_t* c)
{
    ScsiCdb cdb;
    cdb.opcode = c[0];
    cdb.lun = c[1];
    cdb.lba = read_be32(c + 2);  // bytes 2-5, big-endian
    if (cdb.opcode == kScsiRead12) {
        cdb.length = read_be32(c + 6);  // READ(12) transfer length: u32 BE @6-9
    } else {
        cdb.length = read_be16(c + 7);  // READ(10)/READ_TOC length: u16 BE @7-8
    }
    return cdb;
}

ScsiResult ScsiCdTarget::execute(const ScsiCdb& cdb)
{
    switch (cdb.opcode) {
    case kScsiTestUnitReady:
        if (first_test_unit_ready_) {
            first_test_unit_ready_ = false;
            // UNIT ATTENTION / NOT READY TO READY CHANGE, MEDIUM MAY HAVE CHANGED (6/28h/00):
            // the first TUR reports the freshly inserted medium so the host re-reads capacity.
            return check_condition(0x06, 0x28, 0x00);
        }
        return ScsiResult{};  // GOOD, no data

    case kScsiReadCapacity:
        return read_capacity();

    case kScsiRead10:
    case kScsiRead12:
        return read_blocks(cdb);

    case kScsiReadToc:
        return read_toc(cdb);

    case kScsiStartStopUnit:
        // Load/eject is signaled out-of-band by the transport (the IUSB control opcode with a
        // magic LBA), so here we just acknowledge.
        return ScsiResult{};

    case kScsiMediumRemoval:
    default:
        // Prevent/allow medium removal and anything unrecognized: ILLEGAL REQUEST / INVALID
        // COMMAND OPERATION CODE (5/20h/00), matching the H5Viewer's "unsupported command".
        return check_condition(0x05, 0x20, 0x00);
    }
}

ScsiResult ScsiCdTarget::read_capacity() const
{
    // READ CAPACITY(10) data: last-LBA then block length, both u32 big-endian.
    const std::uint64_t capacity = source_.capacity_sectors();
    const std::uint32_t last_lba =
        capacity == 0 ? 0 : static_cast<std::uint32_t>(capacity - 1);

    ScsiResult result;
    result.data.reserve(8);
    push_be32(result.data, last_lba);
    push_be32(result.data, source_.sector_size());
    return result;
}

ScsiResult ScsiCdTarget::read_blocks(const ScsiCdb& cdb)
{
    if (cdb.length == 0) {
        return ScsiResult{};  // zero-length transfer: GOOD, no data
    }

    ScsiResult result;
    result.data.resize(static_cast<std::size_t>(cdb.length) * source_.sector_size());
    if (!source_.read(cdb.lba, cdb.length, result.data.data())) {
        // MEDIUM ERROR / UNRECOVERED READ ERROR (3/11h/00).
        return check_condition(0x03, 0x11, 0x00);
    }
    return result;
}

ScsiResult ScsiCdTarget::read_toc(const ScsiCdb& cdb) const
{
    // A minimal single-session TOC: one data track (#1) plus the lead-out (0xAA), each an
    // 8-byte descriptor, prefixed by the 4-byte TOC header. Matches the H5Viewer's readTOC().
    std::uint8_t toc[20] = {0};
    std::size_t n = 4;
    const bool msf = (cdb.lun & 0x02) != 0;  // request MSF addresses rather than LBA

    toc[2] = 1;  // first track number
    toc[3] = 1;  // last track number

    // Track 1 descriptor: ADR/control 0x14 (data track), address = start of disc.
    toc[n++] = 0;
    toc[n++] = 0x14;
    toc[n++] = 1;
    toc[n++] = 0;
    if (msf) {
        toc[n++] = 0;
        toc[n++] = 0;
        toc[n++] = 2;  // MSF 00:00:02 (the conventional first-sector address)
        toc[n++] = 0;
    } else {
        toc[n++] = 0;
        toc[n++] = 0;
        toc[n++] = 0;
        toc[n++] = 0;  // LBA 0
    }

    // Lead-out (track 0xAA): address = total disc size, expressed in MSF (min:sec:frame at 75
    // frames/sec, with the 150-frame / 2-second pre-gap added like a real CD).
    const std::uint64_t lead_out = source_.capacity_sectors() + 150;
    toc[n++] = 0;
    toc[n++] = 0x16;
    toc[n++] = 0xAA;
    toc[n++] = 0;
    toc[n++] = 0;
    toc[n++] = static_cast<std::uint8_t>(lead_out / 75 / 60);
    toc[n++] = static_cast<std::uint8_t>((lead_out / 75) % 60);
    toc[n++] = static_cast<std::uint8_t>(lead_out % 75);

    // TOC data length header (the count of bytes that follow these 2).
    const std::uint16_t data_length = static_cast<std::uint16_t>(n - 2);
    toc[0] = static_cast<std::uint8_t>(data_length >> 8);
    toc[1] = static_cast<std::uint8_t>(data_length & 0xFF);

    // Return min(available, allocation length): the host first asks with a small allocation to
    // learn the length, then re-asks with enough room.
    const std::size_t copy = std::min<std::size_t>(n, cdb.length);
    ScsiResult result;
    result.data.assign(toc, toc + copy);
    return result;
}

} // namespace hitsc
