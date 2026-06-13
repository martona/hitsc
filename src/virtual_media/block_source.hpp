#pragma once

#include <cstdint>

namespace hitsc {

// A read-only block device the BMC virtual-media transport serves sectors from. The two
// implementations are IsoFileSource (an .iso on disk) and -- later -- a synthesized
// ISO9660 image built from loose files. Backend-agnostic and Qt-free: the MegaRAC (IUSB)
// and ATEN (USB-BOT) transports both drive a BlockSource through the shared ScsiCdTarget.
//
// All reads are synchronous; a BlockSource is meant to run on the dedicated media network
// thread, never the UI or video thread.
class BlockSource {
public:
    virtual ~BlockSource() = default;

    virtual std::uint32_t sector_size() const = 0;       // 2048 for a CD/DVD image
    virtual std::uint64_t capacity_sectors() const = 0;  // total sectors (so last LBA = this - 1)

    // Fill out[0 .. count * sector_size()) with the `count` sectors starting at `lba`.
    // Reads at or past capacity zero-fill the tail (a CD read past the end returns zeros,
    // not an error). Returns false only on an underlying I/O failure.
    virtual bool read(std::uint64_t lba, std::uint32_t count, std::uint8_t* out) = 0;
};

} // namespace hitsc
