#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace hitsc {

enum class KvmCaptureRecordType : std::uint16_t {
    Metadata = 1,
    IncomingPacket = 2,
    OutgoingMessage = 3,
    Note = 4,
};

class KvmCaptureWriter {
public:
    explicit KvmCaptureWriter(std::string_view path);
    ~KvmCaptureWriter();

    KvmCaptureWriter(const KvmCaptureWriter&) = delete;
    KvmCaptureWriter& operator=(const KvmCaptureWriter&) = delete;
    KvmCaptureWriter(KvmCaptureWriter&&) noexcept;
    KvmCaptureWriter& operator=(KvmCaptureWriter&&) noexcept;

    void write_metadata(std::string_view json);
    void write_note(std::string_view note);
    void write_incoming_packet(
        std::uint16_t type,
        std::uint16_t status,
        const std::vector<std::uint8_t>& payload);
    void write_outgoing_message(const std::vector<std::uint8_t>& bytes);

private:
    void write_record(KvmCaptureRecordType type, const std::vector<std::uint8_t>& payload);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hitsc
