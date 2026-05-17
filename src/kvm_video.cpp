#include "kvm_video.hpp"

#include <cstddef>
#include <stdexcept>

namespace hitsc {
namespace {

std::uint16_t read_le16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    if (offset + 2 > bytes.size()) {
        throw std::runtime_error("truncated little-endian uint16");
    }
    return static_cast<std::uint16_t>(bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t read_le32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size()) {
        throw std::runtime_error("truncated little-endian uint32");
    }
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

} // namespace

std::optional<KvmVideoFrame> KvmVideoAssembler::ingest(const std::vector<std::uint8_t>& packet_payload)
{
    if (packet_payload.size() < 2) {
        throw std::runtime_error("video packet is too short");
    }

    if (expecting_new_frame_) {
        if (packet_payload.size() < 88) {
            throw std::runtime_error("initial video packet is too short");
        }

        const std::size_t header_base = 2;
        current_ = KvmVideoFrame{};
        current_.width = read_le16(packet_payload, header_base + 13);
        current_.height = read_le16(packet_payload, header_base + 15);
        current_.compression_mode = packet_payload[header_base + 42];
        current_.jpeg_table_selector = packet_payload[header_base + 44];
        current_.advance_table_selector = packet_payload[header_base + 47];
        current_.rc4_enable = packet_payload[header_base + 53];
        current_.mode420 = packet_payload[header_base + 55];
        current_.compressed_size = read_le32(packet_payload, header_base + 69);
        current_.compressed.reserve(current_.compressed_size);
        append_fragment(packet_payload, 88);
    } else {
        append_fragment(packet_payload, 2);
    }

    if (current_.compressed.size() > current_.compressed_size) {
        throw std::runtime_error("assembled video frame exceeded declared compressed size");
    }

    if (current_.compressed.size() == current_.compressed_size) {
        expecting_new_frame_ = true;
        return current_;
    }

    expecting_new_frame_ = false;
    return std::nullopt;
}

void KvmVideoAssembler::append_fragment(const std::vector<std::uint8_t>& packet_payload, std::size_t offset)
{
    if (offset > packet_payload.size()) {
        throw std::runtime_error("invalid video fragment offset");
    }
    current_.compressed.insert(
        current_.compressed.end(),
        packet_payload.begin() + static_cast<std::ptrdiff_t>(offset),
        packet_payload.end());
}

std::uint8_t kvm_video_first_block_header(const std::vector<std::uint8_t>& compressed)
{
    if (compressed.size() < 4) {
        return 0xff;
    }
    const std::uint32_t word = static_cast<std::uint32_t>(compressed[0])
        | (static_cast<std::uint32_t>(compressed[1]) << 8)
        | (static_cast<std::uint32_t>(compressed[2]) << 16)
        | (static_cast<std::uint32_t>(compressed[3]) << 24);
    return static_cast<std::uint8_t>((word >> 28) & 0x0f);
}

bool kvm_video_is_supported_first_block(std::uint8_t header)
{
    switch (header) {
    case 0x00:
    case 0x02:
    case 0x04:
    case 0x08:
    case 0x09:
    case 0x0a:
    case 0x0c:
        return true;
    default:
        return false;
    }
}

} // namespace hitsc
