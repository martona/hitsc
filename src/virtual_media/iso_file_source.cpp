#include "virtual_media/iso_file_source.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace hitsc {
namespace {

// ISO9660 stamps "CD001" at the start of the Primary Volume Descriptor (sector 16, one byte
// in). A pure-UDF image instead carries "*OSTA UDF Compliant" in its anchor area. Checking
// both mirrors the H5Viewer's validateISOImage(), which is the behavior the BMC expects.
constexpr std::uint64_t kPvdLba = 16;
constexpr std::uint64_t kUdfLba = 35;
constexpr std::uint64_t kUdfDomainOffset = 217;

bool read_string_at(std::ifstream& file, std::uint64_t size_bytes, std::uint64_t offset,
                    std::size_t length, std::string& out)
{
    if (offset + length > size_bytes) {
        return false;
    }
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset));
    out.resize(length);
    file.read(out.data(), static_cast<std::streamsize>(length));
    return static_cast<std::size_t>(file.gcount()) == length;
}

bool has_iso_signature(std::ifstream& file, std::uint64_t size_bytes)
{
    std::string id;
    if (read_string_at(file, size_bytes, kPvdLba * IsoFileSource::kCdSectorSize + 1, 5, id) &&
        id == "CD001") {
        return true;
    }
    if (read_string_at(file, size_bytes, kUdfLba * IsoFileSource::kCdSectorSize + kUdfDomainOffset,
                       19, id) &&
        id == "*OSTA UDF Compliant") {
        return true;
    }
    return false;
}

} // namespace

IsoFileSource::IsoFileSource(const std::string& path)
    : file_(path, std::ios::binary)
{
    if (!file_) {
        throw std::runtime_error("cannot open ISO file: " + path);
    }
    file_.seekg(0, std::ios::end);
    const std::streamoff end = file_.tellg();
    if (end <= 0) {
        throw std::runtime_error("ISO file is empty: " + path);
    }
    size_bytes_ = static_cast<std::uint64_t>(end);
    // CD images are 2048-aligned; any trailing partial sector is unreachable (floored away).
    capacity_sectors_ = size_bytes_ / kCdSectorSize;
    if (capacity_sectors_ == 0) {
        throw std::runtime_error("ISO file is smaller than one CD sector: " + path);
    }
    if (!has_iso_signature(file_, size_bytes_)) {
        throw std::runtime_error("not a recognizable ISO9660/UDF image: " + path);
    }
}

bool IsoFileSource::read(std::uint64_t lba, std::uint32_t count, std::uint8_t* out)
{
    const std::uint64_t total = static_cast<std::uint64_t>(count) * kCdSectorSize;
    const std::uint64_t start = lba * kCdSectorSize;

    if (start >= size_bytes_) {
        std::memset(out, 0, static_cast<std::size_t>(total));  // entirely past EOF -> zeros
        return true;
    }

    const std::uint64_t to_read = std::min(total, size_bytes_ - start);
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(start));
    file_.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(to_read));
    if (static_cast<std::uint64_t>(file_.gcount()) != to_read) {
        return false;  // genuine I/O error: we asked for in-bounds bytes and got fewer
    }
    if (to_read < total) {
        std::memset(out + to_read, 0, static_cast<std::size_t>(total - to_read));  // zero-pad tail
    }
    return true;
}

} // namespace hitsc
