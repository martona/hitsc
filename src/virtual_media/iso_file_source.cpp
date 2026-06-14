#include "virtual_media/iso_file_source.hpp"

#include <algorithm>
#include <cstdlib>
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

// 64-bit file positioning -- std::fpos / 32-bit seeks can't address a multi-GB ISO.
int seek_to(std::FILE* file, std::uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET);
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET);
#endif
}

std::int64_t file_size(std::FILE* file)
{
#ifdef _WIN32
    if (_fseeki64(file, 0, SEEK_END) != 0) {
        return -1;
    }
    return _ftelli64(file);
#else
    if (fseeko(file, 0, SEEK_END) != 0) {
        return -1;
    }
    return ftello(file);
#endif
}

bool read_string_at(std::FILE* file, std::uint64_t size_bytes, std::uint64_t offset,
                    std::size_t length, std::string& out)
{
    if (offset + length > size_bytes) {
        return false;
    }
    if (seek_to(file, offset) != 0) {
        return false;
    }
    out.resize(length);
    return std::fread(out.data(), 1, length, file) == length;
}

bool has_iso_signature(std::FILE* file, std::uint64_t size_bytes)
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
{
    file_ = std::fopen(path.c_str(), "rb");
    if (file_ == nullptr) {
        throw std::runtime_error("cannot open ISO file: " + path);
    }

    const std::int64_t size = file_size(file_);
    if (size <= 0) {
        std::fclose(file_);
        file_ = nullptr;
        throw std::runtime_error("ISO file is empty or unreadable: " + path);
    }
    size_bytes_ = static_cast<std::uint64_t>(size);
    // CD images are 2048-aligned; any trailing partial sector is unreachable (floored away).
    capacity_sectors_ = size_bytes_ / kCdSectorSize;
    if (capacity_sectors_ == 0) {
        std::fclose(file_);
        file_ = nullptr;
        throw std::runtime_error("ISO file is smaller than one CD sector: " + path);
    }
    if (!has_iso_signature(file_, size_bytes_)) {
        std::fclose(file_);
        file_ = nullptr;
        throw std::runtime_error("not a recognizable ISO9660/UDF image: " + path);
    }
}

IsoFileSource::~IsoFileSource()
{
    if (file_ != nullptr) {
        std::fclose(file_);
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
    if (seek_to(file_, start) != 0) {
        return false;
    }
    const std::size_t got = std::fread(out, 1, static_cast<std::size_t>(to_read), file_);
    if (got != to_read) {
        if (std::getenv("HITSC_DEBUG_ISO") != nullptr) {
            std::fprintf(stderr,
                         "IsoFileSource::read short: lba=%llu count=%u start=%llu to_read=%llu got=%zu size=%llu\n",
                         static_cast<unsigned long long>(lba), count,
                         static_cast<unsigned long long>(start),
                         static_cast<unsigned long long>(to_read), got,
                         static_cast<unsigned long long>(size_bytes_));
        }
        return false;
    }
    if (to_read < total) {
        std::memset(out + to_read, 0, static_cast<std::size_t>(total - to_read));  // zero-pad tail
    }
    return true;
}

} // namespace hitsc
