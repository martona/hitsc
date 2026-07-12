#include "virtual_media/scsi_cd_target.hpp"

#include "virtual_media/block_source.hpp"

#include <algorithm>
#include <cstring>

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

std::uint32_t read_le32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// The largest read we will service. The BMC caps reads at 64 sectors (its MAX_READ_SECTORS),
// so anything beyond a very generous bound is a corrupt/misparsed CDB, not a real request --
// reject it rather than allocate gigabytes.
constexpr std::uint32_t kMaxTransferSectors = 8192;  // 16 MiB

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

// A data-in reply clamped to the CDB allocation length. The full USB-BOT model requires
// honoring the requested length (the host sizes its bulk-in transfer to it); returning more
// than asked would overrun. `alloc == 0` means "return everything" (some CDBs omit a length).
ScsiResult data_in(std::vector<std::uint8_t> full, std::uint32_t alloc)
{
    if (alloc != 0 && full.size() > alloc) {
        full.resize(alloc);
    }
    ScsiResult result;
    result.data = std::move(full);
    return result;
}

} // namespace

ScsiCdb ScsiCdTarget::parse_cdb(const std::uint8_t* c)
{
    ScsiCdb cdb;
    cdb.opcode = c[0];
    cdb.lun = c[1];
    cdb.lba = read_be32(c + 2);  // bytes 2-5, big-endian (both READ(10) and READ(12))
    if (cdb.opcode == kScsiRead12) {
        // READ(12) transfer length is LITTLE-endian on the wire here -- the AMI BMC/H5Viewer
        // convention (the JS reads it with the DataStream's native endianness). This BMC sends
        // host READ(10)s to us as READ(12), so getting this right is not optional. Big-endian
        // here turns an on-wire "02 00 00 00" (2) into 0x02000000 (33M sectors).
        cdb.length = read_le32(c + 6);  // u32 LE @6-9
    } else if (cdb.opcode == kScsiInquiry || cdb.opcode == kScsiRequestSense) {
        // 6-byte CDBs: a single-byte allocation length at byte 4 (bytes, not sectors).
        cdb.length = c[4];
    } else {
        cdb.length = read_be16(c + 7);  // READ(10)/READ_TOC length: u16 BE @7-8
    }
    return cdb;
}

ScsiResult ScsiCdTarget::execute(const ScsiCdb& cdb)
{
    const bool full_emulation = persona_ == ScsiPersona::FullDeviceEmulation;

    // REQUEST SENSE reports the latched sense and must not itself overwrite it, so handle it
    // before the dispatch that latches. (Only reachable under full device emulation.)
    if (full_emulation && cdb.opcode == kScsiRequestSense) {
        return request_sense(cdb);
    }

    ScsiResult result;
    switch (cdb.opcode) {
    case kScsiTestUnitReady:
        if (first_test_unit_ready_) {
            first_test_unit_ready_ = false;
            // UNIT ATTENTION / NOT READY TO READY CHANGE, MEDIUM MAY HAVE CHANGED (6/28h/00):
            // the first TUR reports the freshly inserted medium so the host re-reads capacity.
            result = check_condition(0x06, 0x28, 0x00);
        }
        // else GOOD, no data
        break;

    case kScsiReadCapacity:
        result = read_capacity();
        break;

    case kScsiRead10:
    case kScsiRead12:
        result = read_blocks(cdb);
        break;

    case kScsiReadToc:
        result = read_toc(cdb);
        break;

    case kScsiStartStopUnit:
        // Load/eject is signaled out-of-band by the transport (the IUSB control opcode with a
        // magic LBA), so here we just acknowledge.
        break;

    case kScsiInquiry:
        result = full_emulation ? inquiry(cdb) : check_condition(0x05, 0x20, 0x00);
        break;

    case kScsiGetPerformance:
        result = full_emulation ? get_performance(cdb) : check_condition(0x05, 0x20, 0x00);
        break;

    case kScsiMediumRemoval:
        // Prevent/allow medium removal: harmless to acknowledge under full emulation (the guest
        // locks the door before reads); the BMC-serviced path never sees it, but keep the old
        // behavior of rejecting it there so nothing changes for MegaRAC.
        if (!full_emulation) {
            result = check_condition(0x05, 0x20, 0x00);
        }
        break;

    default:
        // Anything unrecognized: ILLEGAL REQUEST / INVALID COMMAND OPERATION CODE (5/20h/00),
        // matching the H5Viewer's "unsupported command".
        result = check_condition(0x05, 0x20, 0x00);
        break;
    }

    // Latch sense from a CHECK CONDITION so a following REQUEST SENSE can retrieve it (the full
    // USB-BOT model has no autosense). Harmless for BmcServiced -- it never issues REQUEST SENSE.
    if (result.status != 0) {
        last_sense_key_ = result.sense_key;
        last_asc_ = result.asc;
        last_ascq_ = result.ascq;
    }
    return result;
}

ScsiResult ScsiCdTarget::inquiry(const ScsiCdb& cdb) const
{
    // Standard INQUIRY data for a removable CD-ROM, lifted verbatim from the ATEN H5Viewer
    // (isohandler.js ab_sbc3_CDROMinquiry): peripheral type 5 (CD/DVD), RMB set, 31 additional
    // bytes, vendor "ATEN", product "Virtual CDROM", revision "YS0J".
    static const std::uint8_t kInquiry[36] = {
        0x05, 0x80, 0x00, 0x21, 0x1F, 0x00, 0x00, 0x00,
        'A', 'T', 'E', 'N', ' ', ' ', ' ', ' ',
        'V', 'i', 'r', 't', 'u', 'a', 'l', ' ', 'C', 'D', 'R', 'O', 'M', ' ', ' ', ' ',
        'Y', 'S', '0', 'J',
    };
    return data_in(std::vector<std::uint8_t>(std::begin(kInquiry), std::end(kInquiry)), cdb.length);
}

ScsiResult ScsiCdTarget::request_sense(const ScsiCdb& cdb)
{
    // Fixed-format sense data (response code 0x70), reporting the latched sense from the last
    // CHECK CONDITION, then clearing it to NO SENSE. Mirrors the H5Viewer's ab_ReqSense_success
    // template with the key/ASC/ASCQ filled in.
    std::vector<std::uint8_t> sense(18, 0);
    sense[0] = 0x70;             // current error, fixed format
    sense[2] = last_sense_key_;  // sense key
    sense[7] = 0x0A;             // additional sense length (10 -> total 18)
    sense[12] = last_asc_;
    sense[13] = last_ascq_;

    last_sense_key_ = 0;
    last_asc_ = 0;
    last_ascq_ = 0;
    return data_in(std::move(sense), cdb.length);
}

ScsiResult ScsiCdTarget::get_performance(const ScsiCdb& /*cdb*/) const
{
    // GET PERFORMANCE performance-data header, lifted verbatim from the ATEN H5Viewer
    // (isohandler.js ab_sbc3_CDROM_OP_AC): a single nominal-performance descriptor. The
    // reference sends these 20 bytes unconditionally, so we do too.
    static const std::uint8_t kPerformance[20] = {
        0x00, 0x00, 0x00, 0x14, 0x02, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x50,
        0x00, 0x00, 0x00, 0x2B,
    };
    return data_in(std::vector<std::uint8_t>(std::begin(kPerformance), std::end(kPerformance)), 0);
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
    if (cdb.length > kMaxTransferSectors) {
        // ILLEGAL REQUEST / INVALID FIELD IN CDB: never trust an absurd length enough to size
        // an allocation from it.
        return check_condition(0x05, 0x24, 0x00);
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

    // TOC data length header (the count of bytes that follow these 2), written after the clamp
    // to the allocation length -- the H5Viewer writes it post-truncation too.
    const std::size_t copy = std::min<std::size_t>(n, cdb.length);
    const std::uint16_t data_length = static_cast<std::uint16_t>(copy >= 2 ? copy - 2 : 0);
    toc[0] = static_cast<std::uint8_t>(data_length >> 8);
    toc[1] = static_cast<std::uint8_t>(data_length & 0xFF);

    // Answer with EXACTLY the allocation length, zero-padded past the real TOC. That is what
    // the H5Viewer sends (it sizes its reply buffer to the request), so it is the only response
    // shape the BMC's USB gadget has ever been exercised with; a shorter data-in transfer is
    // legal SCSI but untested territory.
    ScsiResult result;
    result.data.assign(cdb.length, 0);
    if (copy > 0) {
        std::memcpy(result.data.data(), toc, copy);
    }
    return result;
}

} // namespace hitsc
