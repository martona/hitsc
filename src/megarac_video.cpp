#include "megarac_video.hpp"

#include "megarac_protocol.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace hitsc {
namespace {

constexpr std::uint32_t kMaxCompressedFrameSize = 128U * 1024U * 1024U;
constexpr int kMaxFrameDimension = 8192;

} // namespace

std::optional<MegaracVideoFrame> MegaracVideoAssembler::ingest(const std::vector<std::uint8_t>& packet_payload)
{
    if (packet_payload.size() < 2) {
        throw std::runtime_error("video packet is too short");
    }

    if (expecting_new_frame_) {
        if (packet_payload.size() < 88) {
            throw std::runtime_error("initial video packet is too short");
        }

        const std::size_t header_base = 2;
        current_ = MegaracVideoFrame{};
        current_.width = load_le16(packet_payload, header_base + 13);
        current_.height = load_le16(packet_payload, header_base + 15);
        current_.compression_mode = packet_payload[header_base + 42];
        current_.jpeg_table_selector = packet_payload[header_base + 44];
        current_.advance_table_selector = packet_payload[header_base + 47];
        current_.rc4_enable = packet_payload[header_base + 53];
        current_.mode420 = packet_payload[header_base + 55];
        current_.compressed_size = load_le32(packet_payload, header_base + 69);
        if (current_.width <= 0 || current_.height <= 0 ||
            current_.width > kMaxFrameDimension || current_.height > kMaxFrameDimension) {
            throw std::runtime_error("video packet declared implausible frame dimensions");
        }
        if (current_.compressed_size == 0 || current_.compressed_size > kMaxCompressedFrameSize) {
            throw std::runtime_error("video packet declared implausible compressed size");
        }
        current_.compressed.reserve(std::min<std::size_t>(current_.compressed_size, packet_payload.size()));
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

void MegaracVideoAssembler::append_fragment(const std::vector<std::uint8_t>& packet_payload, std::size_t offset)
{
    if (offset > packet_payload.size()) {
        throw std::runtime_error("invalid video fragment offset");
    }
    current_.compressed.insert(
        current_.compressed.end(),
        packet_payload.begin() + static_cast<std::ptrdiff_t>(offset),
        packet_payload.end());
}

std::uint8_t megarac_video_first_block_header(const std::vector<std::uint8_t>& compressed)
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

bool megarac_video_is_supported_first_block(std::uint8_t header)
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

AspeedDecodeOptions make_megarac_aspeed_decode_options(const MegaracVideoFrame& frame)
{
    AspeedDecodeOptions options;
    options.width = frame.width;
    options.height = frame.height;
    options.mode420 = frame.mode420;
    options.jpeg_table_selector = frame.jpeg_table_selector;
    options.advance_table_selector = frame.advance_table_selector;
    return options;
}

} // namespace hitsc
