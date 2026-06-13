#pragma once

#include "virtual_media/block_source.hpp"

#include <cstdint>
#include <fstream>
#include <string>

namespace hitsc {

// A BlockSource backed by an .iso file on disk: 2048-byte CD sectors, capacity derived from
// the file size, reads served by seeking into the file (reads past EOF zero-pad). The
// constructor validates the ISO9660/UDF signature so a non-image file fails the mount with a
// clear error instead of feeding the BMC garbage. Synchronous I/O -- runs on the media
// network thread.
class IsoFileSource : public BlockSource {
public:
    static constexpr std::uint32_t kCdSectorSize = 2048;

    // Opens and validates `path`. Throws std::runtime_error if it can't be opened, is empty,
    // is smaller than one sector, or isn't a recognizable ISO9660/UDF image.
    explicit IsoFileSource(const std::string& path);

    std::uint32_t sector_size() const override { return kCdSectorSize; }
    std::uint64_t capacity_sectors() const override { return capacity_sectors_; }
    bool read(std::uint64_t lba, std::uint32_t count, std::uint8_t* out) override;

private:
    std::ifstream file_;
    std::uint64_t size_bytes_ = 0;
    std::uint64_t capacity_sectors_ = 0;
};

} // namespace hitsc
