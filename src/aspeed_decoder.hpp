#pragma once

#include <cstdint>
#include <vector>

namespace hitsc {

struct AspeedDecodeOptions {
    int width = 0;
    int height = 0;
    unsigned mode420 = 0;
    unsigned jpeg_table_selector = 0;
    unsigned chroma_table_selector = 0;
    unsigned advance_table_selector = 0;
    unsigned advance_chroma_table_selector = 0;
    bool use_separate_chroma_selectors = false;
};

class AspeedDecoder {
public:
    AspeedDecoder();

    std::vector<std::uint8_t> decode_rgba(
        const AspeedDecodeOptions& options,
        const std::vector<std::uint8_t>& compressed,
        const std::vector<std::uint8_t>* previous_rgba = nullptr);
};

} // namespace hitsc
