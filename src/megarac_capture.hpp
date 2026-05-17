#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace hitsc {

enum class MegaracCaptureRecordType : std::uint16_t {
    Metadata = 1,
    IncomingPacket = 2,
    OutgoingMessage = 3,
    Note = 4,
};

class MegaracCaptureWriter {
public:
    explicit MegaracCaptureWriter(std::string_view path);
    ~MegaracCaptureWriter();

    MegaracCaptureWriter(const MegaracCaptureWriter&) = delete;
    MegaracCaptureWriter& operator=(const MegaracCaptureWriter&) = delete;
    MegaracCaptureWriter(MegaracCaptureWriter&&) noexcept;
    MegaracCaptureWriter& operator=(MegaracCaptureWriter&&) noexcept;

    void write_metadata(std::string_view json);
    void write_note(std::string_view note);
    void write_incoming_packet(
        std::uint16_t type,
        std::uint16_t status,
        const std::vector<std::uint8_t>& payload);
    void write_outgoing_message(const std::vector<std::uint8_t>& bytes);

private:
    void write_record(MegaracCaptureRecordType type, const std::vector<std::uint8_t>& payload);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hitsc
