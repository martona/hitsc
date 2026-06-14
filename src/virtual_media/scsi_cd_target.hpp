#pragma once

#include <cstdint>
#include <vector>

namespace hitsc {

class BlockSource;

// SCSI opcodes a BMC virtual-CD issues. (CD redirection only ever needs this handful.)
enum : std::uint8_t {
    kScsiTestUnitReady = 0x00,
    kScsiStartStopUnit = 0x1B,
    kScsiMediumRemoval = 0x1E,  // PREVENT/ALLOW MEDIUM REMOVAL
    kScsiReadCapacity = 0x25,
    kScsiRead10 = 0x28,
    kScsiReadToc = 0x43,
    kScsiRead12 = 0xA8,
};

// A parsed SCSI command descriptor block (the subset a CD target sees). SCSI fields are
// big-endian on the wire.
struct ScsiCdb {
    std::uint8_t opcode = 0;
    std::uint8_t lun = 0;     // byte 1 (the BMC packs LUN/flags here; READ_TOC's MSF bit lives in it)
    std::uint64_t lba = 0;    // bytes 2-5, big-endian
    // Transfer/allocation length. Endianness is asymmetric (AMI BMC convention): READ(10) /
    // READ_TOC use a big-endian u16 @7-8; READ(12) uses a LITTLE-endian u32 @6-9.
    std::uint32_t length = 0;
};

// One serviced command's result. status: 0 = GOOD, 1 = CHECK CONDITION (then sense_key/asc/ascq
// describe the condition). `data` is the payload to return (empty for non-data commands).
struct ScsiResult {
    std::uint8_t status = 0;
    std::uint8_t sense_key = 0;
    std::uint8_t asc = 0;
    std::uint8_t ascq = 0;
    std::vector<std::uint8_t> data;
};

// Transport-independent CD/DVD SCSI responder. Mirrors the AMI H5Viewer's CD target: it
// answers the small command set a BMC virtual-CD issues, reading payload sectors from a
// BlockSource. The MegaRAC (IUSB) and, later, ATEN (USB-BOT) transports both wrap this; it
// knows nothing about either framing. 2048-byte sectors throughout.
class ScsiCdTarget {
public:
    explicit ScsiCdTarget(BlockSource& source) : source_(source) {}

    // Parse a CDB using SCSI big-endian field order. `cdb` must point at >= 12 readable bytes
    // (the command region in the IUSB/CBW packet), starting at the opcode byte.
    static ScsiCdb parse_cdb(const std::uint8_t* cdb);

    // Service one command, reading from the BlockSource as needed.
    ScsiResult execute(const ScsiCdb& cdb);

private:
    ScsiResult read_capacity() const;
    ScsiResult read_blocks(const ScsiCdb& cdb);
    ScsiResult read_toc(const ScsiCdb& cdb) const;

    BlockSource& source_;
    bool first_test_unit_ready_ = true;  // first TUR reports a fresh-medium UNIT ATTENTION
};

} // namespace hitsc
