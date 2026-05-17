#include "kvm_capture_decode.hpp"

#include "aspeed_decoder.hpp"
#include "kvm_capture.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace hitsc {
namespace {

constexpr std::array<char, 16> kMagic = {'H', 'I', 'T', 'S', 'C', 'K', 'V', 'M', 'C', 'A', 'P', '0', '0', '1', '\r', '\n'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint16_t kCmdVideoPackets = 25;

struct CaptureRecord {
    KvmCaptureRecordType type = KvmCaptureRecordType::Note;
    std::uint64_t timestamp_us = 0;
    std::vector<std::uint8_t> payload;
};

struct KvmPacket {
    std::uint16_t type = 0;
    std::uint16_t status = 0;
    std::vector<std::uint8_t> payload;
};

struct VideoFrame {
    int width = 0;
    int height = 0;
    std::uint8_t compression_mode = 0;
    std::uint8_t jpeg_table_selector = 0;
    std::uint8_t advance_table_selector = 0;
    std::uint8_t rc4_enable = 0;
    std::uint8_t mode420 = 0;
    std::uint32_t compressed_size = 0;
    std::vector<std::uint8_t> compressed;
};

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

std::uint16_t read_u16(std::ifstream& input)
{
    unsigned char bytes[2]{};
    input.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!input) {
        throw std::runtime_error("truncated capture record header");
    }
    return static_cast<std::uint16_t>(bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8));
}

std::uint32_t read_u32(std::ifstream& input)
{
    unsigned char bytes[4]{};
    input.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!input) {
        throw std::runtime_error("truncated capture file");
    }
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t read_u64(std::ifstream& input)
{
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        unsigned char byte = 0;
        input.read(reinterpret_cast<char*>(&byte), sizeof(byte));
        if (!input) {
            throw std::runtime_error("truncated capture file");
        }
        value |= static_cast<std::uint64_t>(byte) << shift;
    }
    return value;
}

std::vector<std::uint8_t> read_payload(std::ifstream& input, std::uint32_t size)
{
    std::vector<std::uint8_t> payload(size);
    if (size != 0) {
        input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!input) {
            throw std::runtime_error("truncated capture payload");
        }
    }
    return payload;
}

std::optional<CaptureRecord> read_record(std::ifstream& input)
{
    const int next = input.peek();
    if (next == std::char_traits<char>::eof()) {
        return std::nullopt;
    }

    CaptureRecord record;
    record.type = static_cast<KvmCaptureRecordType>(read_u16(input));
    (void)read_u16(input);
    record.timestamp_us = read_u64(input);
    record.payload = read_payload(input, read_u32(input));
    return record;
}

KvmPacket parse_kvm_packet(const std::vector<std::uint8_t>& payload)
{
    if (payload.size() < 8) {
        throw std::runtime_error("truncated captured KVM packet");
    }

    KvmPacket packet;
    packet.type = read_le16(payload, 0);
    packet.status = read_le16(payload, 6);
    const std::uint32_t declared_size = read_le32(payload, 2);
    if (declared_size != payload.size() - 8) {
        throw std::runtime_error("captured KVM packet payload size mismatch");
    }
    packet.payload.assign(payload.begin() + 8, payload.end());
    return packet;
}

std::string frame_name(int frame_index)
{
    std::ostringstream name;
    name << "frame_" << std::setw(6) << std::setfill('0') << frame_index << ".bmp";
    return name.str();
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

void append_i32(std::vector<std::uint8_t>& bytes, std::int32_t value)
{
    append_u32(bytes, static_cast<std::uint32_t>(value));
}

void write_bmp(
    const std::filesystem::path& path,
    int width,
    int height,
    const std::vector<std::uint8_t>& rgba)
{
    if (rgba.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) {
        throw std::runtime_error("decoded frame buffer size does not match dimensions");
    }

    const std::uint32_t row_bytes = static_cast<std::uint32_t>(((width * 3) + 3) & ~3);
    const std::uint32_t pixel_bytes = row_bytes * static_cast<std::uint32_t>(height);
    const std::uint32_t file_size = 14 + 40 + pixel_bytes;

    std::vector<std::uint8_t> bytes;
    bytes.reserve(file_size);
    bytes.push_back('B');
    bytes.push_back('M');
    append_u32(bytes, file_size);
    append_u16(bytes, 0);
    append_u16(bytes, 0);
    append_u32(bytes, 14 + 40);

    append_u32(bytes, 40);
    append_i32(bytes, width);
    append_i32(bytes, -height);
    append_u16(bytes, 1);
    append_u16(bytes, 24);
    append_u32(bytes, 0);
    append_u32(bytes, pixel_bytes);
    append_i32(bytes, 2835);
    append_i32(bytes, 2835);
    append_u32(bytes, 0);
    append_u32(bytes, 0);

    std::vector<std::uint8_t> row(row_bytes);
    for (int y = 0; y < height; ++y) {
        std::fill(row.begin(), row.end(), 0);
        for (int x = 0; x < width; ++x) {
            const std::size_t source = (static_cast<std::size_t>(y) * width + x) * 4;
            const std::size_t target = static_cast<std::size_t>(x) * 3;
            row[target + 0] = rgba[source + 2];
            row[target + 1] = rgba[source + 1];
            row[target + 2] = rgba[source + 0];
        }
        bytes.insert(bytes.end(), row.begin(), row.end());
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to open BMP output: " + path.string());
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("failed to write BMP output: " + path.string());
    }
}

std::uint8_t first_block_header(const std::vector<std::uint8_t>& compressed)
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

bool is_supported_first_block(std::uint8_t header)
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

class VideoAssembler {
public:
    std::optional<VideoFrame> ingest(const std::vector<std::uint8_t>& payload)
    {
        if (payload.size() < 2) {
            throw std::runtime_error("video packet is too short");
        }

        if (expecting_new_frame_) {
            if (payload.size() < 88) {
                throw std::runtime_error("initial video packet is too short");
            }

            const std::size_t header_base = 2;
            current_ = VideoFrame{};
            current_.width = read_le16(payload, header_base + 13);
            current_.height = read_le16(payload, header_base + 15);
            current_.compression_mode = payload[header_base + 42];
            current_.jpeg_table_selector = payload[header_base + 44];
            current_.advance_table_selector = payload[header_base + 47];
            current_.rc4_enable = payload[header_base + 53];
            current_.mode420 = payload[header_base + 55];
            current_.compressed_size = read_le32(payload, header_base + 69);
            current_.compressed.reserve(current_.compressed_size);
            append_fragment(payload, 88);
        } else {
            append_fragment(payload, 2);
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

private:
    void append_fragment(const std::vector<std::uint8_t>& payload, std::size_t offset)
    {
        if (offset > payload.size()) {
            throw std::runtime_error("invalid video fragment offset");
        }
        current_.compressed.insert(current_.compressed.end(), payload.begin() + static_cast<std::ptrdiff_t>(offset), payload.end());
    }

    bool expecting_new_frame_ = true;
    VideoFrame current_;
};

void validate_capture_header(std::ifstream& input)
{
    std::array<char, 16> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || magic != kMagic) {
        throw std::runtime_error("input is not a hitsc KVM capture file");
    }

    const std::uint32_t version = read_u32(input);
    if (version != kFormatVersion) {
        throw std::runtime_error("unsupported KVM capture format version");
    }
}

} // namespace

void decode_kvm_capture(const KvmCaptureDecodeOptions& options)
{
    if (options.input_path.empty()) {
        throw std::invalid_argument("missing capture input path");
    }
    if (options.output_directory.empty()) {
        throw std::invalid_argument("missing output directory");
    }
    if (options.frame_limit < 0) {
        throw std::invalid_argument("frame limit must be non-negative");
    }

    std::ifstream input(options.input_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open capture input: " + options.input_path);
    }
    validate_capture_header(input);

    const std::filesystem::path output_directory{options.output_directory};
    std::filesystem::create_directories(output_directory);

    AspeedDecoder decoder;
    VideoAssembler assembler;
    int video_packet_count = 0;
    int frame_count = 0;
    int decoded_frame_count = 0;
    int skipped_frame_count = 0;
    int framebuffer_width = 0;
    int framebuffer_height = 0;
    std::vector<std::uint8_t> framebuffer;

    while (const std::optional<CaptureRecord> record = read_record(input)) {
        if (record->type != KvmCaptureRecordType::IncomingPacket) {
            continue;
        }

        const KvmPacket packet = parse_kvm_packet(record->payload);
        if (packet.type != kCmdVideoPackets) {
            continue;
        }

        ++video_packet_count;
        const std::optional<VideoFrame> frame = assembler.ingest(packet.payload);
        if (!frame) {
            continue;
        }

        ++frame_count;
        const std::uint8_t block_header = first_block_header(frame->compressed);
        if (frame->rc4_enable != 0 || !is_supported_first_block(block_header)) {
            ++skipped_frame_count;
            std::cerr << "hitsc: skipped frame #" << frame_count
                      << " " << frame->width << "x" << frame->height
                      << " compression=" << static_cast<int>(frame->compression_mode)
                      << " rc4=" << static_cast<int>(frame->rc4_enable)
                      << " first-block=0x" << std::hex << static_cast<int>(block_header) << std::dec
                      << '\n';
            continue;
        }

        const AspeedDecodeOptions decode_options{
            frame->width,
            frame->height,
            frame->mode420,
            frame->jpeg_table_selector,
            frame->advance_table_selector,
        };

        const bool can_use_previous =
            framebuffer_width == frame->width
            && framebuffer_height == frame->height
            && framebuffer.size() == static_cast<std::size_t>(frame->width) * static_cast<std::size_t>(frame->height) * 4;
        std::vector<std::uint8_t> rgba =
            decoder.decode_rgba(decode_options, frame->compressed, can_use_previous ? &framebuffer : nullptr);
        framebuffer = rgba;
        framebuffer_width = frame->width;
        framebuffer_height = frame->height;

        const std::filesystem::path output_path = output_directory / frame_name(frame_count);
        write_bmp(output_path, frame->width, frame->height, rgba);
        ++decoded_frame_count;

        std::cerr << "hitsc: wrote frame #" << frame_count
                  << " " << frame->width << "x" << frame->height
                  << " bytes=" << frame->compressed.size()
                  << " mode420=" << static_cast<int>(frame->mode420)
                  << " compression=" << static_cast<int>(frame->compression_mode)
                  << " -> " << output_path.string() << '\n';

        if (options.frame_limit != 0 && decoded_frame_count >= options.frame_limit) {
            break;
        }
    }

    std::cerr << "hitsc: decoded " << decoded_frame_count
              << " frame(s), skipped " << skipped_frame_count
              << ", saw " << video_packet_count << " video packet(s)\n";
}

} // namespace hitsc
