#include "aspeed_decoder.hpp"

#include <limits>
#include <mutex>
#include <stdexcept>

extern "C" {
void init(void);
void decode(
    unsigned long* input,
    int length,
    unsigned char* output,
    int width,
    int height,
    unsigned mode420,
    unsigned selector,
    unsigned advance_selector);
void decode_ext(
    unsigned long* input,
    int length,
    unsigned char* output,
    int width,
    int height,
    unsigned mode420,
    unsigned selector,
    unsigned chroma_selector,
    unsigned advance_selector,
    unsigned advance_chroma_selector);
}

namespace hitsc {
namespace {

std::once_flag g_aspeed_init_once;

void append_le_word(std::vector<unsigned long>& words, std::size_t word_index, std::uint8_t byte, int byte_index)
{
    words[word_index] |= static_cast<unsigned long>(byte) << (byte_index * 8);
}

} // namespace

AspeedDecoder::AspeedDecoder()
{
    std::call_once(g_aspeed_init_once, [] { init(); });
}

std::vector<std::uint8_t> AspeedDecoder::decode_rgba(
    const AspeedDecodeOptions& options,
    const std::vector<std::uint8_t>& compressed,
    const std::vector<std::uint8_t>* previous_rgba)
{
    if (options.width <= 0 || options.height <= 0) {
        throw std::invalid_argument("ASPEED frame has invalid dimensions");
    }
    if (options.width > 8192 || options.height > 8192) {
        throw std::invalid_argument("ASPEED frame dimensions are implausibly large");
    }
    if (compressed.empty()) {
        throw std::invalid_argument("ASPEED compressed frame is empty");
    }
    if (compressed.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("ASPEED compressed frame is too large");
    }

    std::vector<unsigned long> input_words((compressed.size() + 3) / 4 + 2);
    for (std::size_t i = 0; i < compressed.size(); ++i) {
        append_le_word(input_words, i / 4, compressed[i], static_cast<int>(i % 4));
    }

    const std::size_t output_size =
        static_cast<std::size_t>(options.width) * static_cast<std::size_t>(options.height) * 4;
    std::vector<std::uint8_t> output(output_size, 0xff);
    if (previous_rgba != nullptr) {
        if (previous_rgba->size() != output_size) {
            throw std::invalid_argument("previous ASPEED frame buffer size does not match dimensions");
        }
        output = *previous_rgba;
    }

    const unsigned chroma_selector =
        options.use_separate_chroma_selectors ? options.chroma_table_selector : options.jpeg_table_selector;
    const unsigned advance_chroma_selector = options.use_separate_chroma_selectors
        ? options.advance_chroma_table_selector
        : options.advance_table_selector;

    decode_ext(
        input_words.data(),
        static_cast<int>(compressed.size()),
        output.data(),
        options.width,
        options.height,
        options.mode420,
        options.jpeg_table_selector,
        chroma_selector,
        options.advance_table_selector,
        advance_chroma_selector);

    return output;
}

} // namespace hitsc
