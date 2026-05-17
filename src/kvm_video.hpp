#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace hitsc {

struct KvmVideoFrame {
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

class KvmVideoAssembler {
public:
    std::optional<KvmVideoFrame> ingest(const std::vector<std::uint8_t>& packet_payload);

private:
    void append_fragment(const std::vector<std::uint8_t>& packet_payload, std::size_t offset);

    bool expecting_new_frame_ = true;
    KvmVideoFrame current_;
};

std::uint8_t kvm_video_first_block_header(const std::vector<std::uint8_t>& compressed);
bool kvm_video_is_supported_first_block(std::uint8_t header);

} // namespace hitsc
