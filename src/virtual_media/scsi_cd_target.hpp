#pragma once

#include <cstdint>
#include <vector>

namespace hitsc {

class BlockSource;

// SCSI opcodes a BMC virtual-CD issues. (CD redirection only ever needs this handful.)
enum : std::uint8_t {
    kScsiTestUnitReady = 0x00,
    kScsiRequestSense = 0x03,   // full device emulation only (ATEN); the host retrieves sense itself
    kScsiInquiry = 0x12,        // full device emulation only (ATEN)
    kScsiStartStopUnit = 0x1B,
    kScsiMediumRemoval = 0x1E,  // PREVENT/ALLOW MEDIUM REMOVAL
    kScsiReadCapacity = 0x25,
    kScsiRead10 = 0x28,
    kScsiReadToc = 0x43,
    kScsiGetPerformance = 0xAC,  // full device emulation only (ATEN)
    kScsiRead12 = 0xA8,
};

// How much of the SCSI target the transport expects us to be.
//
//  - BmcServiced: the BMC answers device-identity commands (INQUIRY, MODE SENSE, REQUEST
//    SENSE) itself and relays only the data-movement subset to us. This is MegaRAC's IUSB
//    model; unrecognized opcodes get CHECK CONDITION / INVALID COMMAND.
//  - FullDeviceEmulation: we ARE the USB mass-storage device, so the guest's INQUIRY /
//    REQUEST SENSE / GET PERFORMANCE come all the way down to us and we must answer them.
//    This is ATEN's USB-BOT model. Requires latching sense so a CHECK CONDITION can be
//    retrieved by a following REQUEST SENSE.
enum class ScsiPersona {
    BmcServiced,
    FullDeviceEmulation,
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
    explicit ScsiCdTarget(BlockSource& source, ScsiPersona persona = ScsiPersona::BmcServiced)
        : source_(source)
        , persona_(persona)
    {
    }

    // Parse a CDB using SCSI big-endian field order. `cdb` must point at >= 12 readable bytes
    // (the command region in the IUSB/CBW packet), starting at the opcode byte.
    static ScsiCdb parse_cdb(const std::uint8_t* cdb);

    // Service one command, reading from the BlockSource as needed.
    ScsiResult execute(const ScsiCdb& cdb);

private:
    ScsiResult read_capacity() const;
    ScsiResult read_blocks(const ScsiCdb& cdb);
    ScsiResult read_toc(const ScsiCdb& cdb) const;
    ScsiResult inquiry(const ScsiCdb& cdb) const;         // FullDeviceEmulation only
    ScsiResult request_sense(const ScsiCdb& cdb);         // FullDeviceEmulation only
    ScsiResult get_performance(const ScsiCdb& cdb) const;  // FullDeviceEmulation only

    BlockSource& source_;
    ScsiPersona persona_;
    bool first_test_unit_ready_ = true;  // first TUR reports a fresh-medium UNIT ATTENTION

    // Latched sense from the most recent CHECK CONDITION, retrieved by a following REQUEST
    // SENSE (FullDeviceEmulation). Cleared to NO SENSE once reported.
    std::uint8_t last_sense_key_ = 0;
    std::uint8_t last_asc_ = 0;
    std::uint8_t last_ascq_ = 0;
};

} // namespace hitsc
