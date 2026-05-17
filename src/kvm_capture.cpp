#include "kvm_capture.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace hitsc {
namespace {

constexpr char kMagic[] = {'H', 'I', 'T', 'S', 'C', 'K', 'V', 'M', 'C', 'A', 'P', '0', '0', '1', '\r', '\n'};
constexpr std::uint32_t kFormatVersion = 1;

void write_bytes(std::ofstream& output, const void* data, std::size_t size)
{
    output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!output) {
        throw std::runtime_error("failed to write KVM capture file");
    }
}

void write_u16(std::ofstream& output, std::uint16_t value)
{
    const unsigned char bytes[]{
        static_cast<unsigned char>(value & 0xff),
        static_cast<unsigned char>((value >> 8) & 0xff),
    };
    write_bytes(output, bytes, sizeof(bytes));
}

void write_u32(std::ofstream& output, std::uint32_t value)
{
    const unsigned char bytes[]{
        static_cast<unsigned char>(value & 0xff),
        static_cast<unsigned char>((value >> 8) & 0xff),
        static_cast<unsigned char>((value >> 16) & 0xff),
        static_cast<unsigned char>((value >> 24) & 0xff),
    };
    write_bytes(output, bytes, sizeof(bytes));
}

void write_u64(std::ofstream& output, std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8) {
        const unsigned char byte = static_cast<unsigned char>((value >> shift) & 0xff);
        write_bytes(output, &byte, sizeof(byte));
    }
}

std::uint32_t checked_size(std::size_t size, std::string_view context)
{
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string(context) + " is too large for KVM capture format");
    }
    return static_cast<std::uint32_t>(size);
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

} // namespace

class KvmCaptureWriter::Impl {
public:
    explicit Impl(std::string_view path)
        : start_(std::chrono::steady_clock::now())
    {
        const std::filesystem::path output_path{std::string(path)};
        if (output_path.empty()) {
            throw std::invalid_argument("capture path is empty");
        }
        if (output_path.has_parent_path()) {
            std::filesystem::create_directories(output_path.parent_path());
        }

        output_.open(output_path, std::ios::binary | std::ios::trunc);
        if (!output_) {
            throw std::runtime_error("failed to open KVM capture file: " + output_path.string());
        }

        write_bytes(output_, kMagic, sizeof(kMagic));
        write_u32(output_, kFormatVersion);
    }

    void write_record(KvmCaptureRecordType type, const std::vector<std::uint8_t>& payload)
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        const auto timestamp_us =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());

        write_u16(output_, static_cast<std::uint16_t>(type));
        write_u16(output_, 0);
        write_u64(output_, timestamp_us);
        write_u32(output_, checked_size(payload.size(), "capture record"));
        if (!payload.empty()) {
            write_bytes(output_, payload.data(), payload.size());
        }
        output_.flush();
    }

private:
    std::ofstream output_;
    std::chrono::steady_clock::time_point start_;
};

KvmCaptureWriter::KvmCaptureWriter(std::string_view path)
    : impl_(std::make_unique<Impl>(path))
{
}

KvmCaptureWriter::~KvmCaptureWriter() = default;
KvmCaptureWriter::KvmCaptureWriter(KvmCaptureWriter&&) noexcept = default;
KvmCaptureWriter& KvmCaptureWriter::operator=(KvmCaptureWriter&&) noexcept = default;

void KvmCaptureWriter::write_metadata(std::string_view json)
{
    const std::vector<std::uint8_t> payload(json.begin(), json.end());
    write_record(KvmCaptureRecordType::Metadata, payload);
}

void KvmCaptureWriter::write_note(std::string_view note)
{
    const std::vector<std::uint8_t> payload(note.begin(), note.end());
    write_record(KvmCaptureRecordType::Note, payload);
}

void KvmCaptureWriter::write_incoming_packet(
    std::uint16_t type,
    std::uint16_t status,
    const std::vector<std::uint8_t>& payload)
{
    std::vector<std::uint8_t> packet;
    packet.reserve(8 + payload.size());
    append_u16(packet, type);
    append_u32(packet, checked_size(payload.size(), "KVM packet payload"));
    append_u16(packet, status);
    packet.insert(packet.end(), payload.begin(), payload.end());
    write_record(KvmCaptureRecordType::IncomingPacket, packet);
}

void KvmCaptureWriter::write_outgoing_message(const std::vector<std::uint8_t>& bytes)
{
    write_record(KvmCaptureRecordType::OutgoingMessage, bytes);
}

void KvmCaptureWriter::write_record(KvmCaptureRecordType type, const std::vector<std::uint8_t>& payload)
{
    impl_->write_record(type, payload);
}

} // namespace hitsc
