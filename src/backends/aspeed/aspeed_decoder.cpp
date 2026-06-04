// Diagnostic toggle: uncomment to log per-frame ASPEED decode telemetry to
// stderr and ./hitsc_aspeed_debug.log — Source vs Destination dims, the 4-bit
// block-code histogram, and how each frame terminated (clean FRAME_END vs the
// runaway guard, which signals a bitstream desync). See the hitsc-megarac-decode
// investigation notes. Leave commented for normal builds.
// #define HITSC_ASPEED_DEBUG 1

#include "aspeed_decoder.hpp"

#include <limits>
#include <mutex>
#include <stdexcept>

#ifdef HITSC_ASPEED_DEBUG
#include <fstream>
#include <iostream>
#include <sstream>
#endif

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
    unsigned advance_chroma_selector,
    unsigned mapping);

#ifdef HITSC_ASPEED_DEBUG
extern int g_aspeed_block_code_counts[16];
extern int g_aspeed_iterations;
extern int g_aspeed_termination;
extern int g_aspeed_final_mbx;
extern int g_aspeed_final_mby;
extern int g_aspeed_bad_selector;
#endif
}

namespace hitsc {
namespace {

std::once_flag g_aspeed_init_once;
std::mutex g_aspeed_decode_mutex;

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

    const std::size_t output_size =
        static_cast<std::size_t>(options.width) * static_cast<std::size_t>(options.height) * 4;
    std::vector<std::uint8_t> output(output_size, 0xff);
    if (previous_rgba != nullptr) {
        if (previous_rgba->size() != output_size) {
            throw std::invalid_argument("previous ASPEED frame buffer size does not match dimensions");
        }
        output = *previous_rgba;
    }

    decode_rgba_into(options, compressed, output);

    return output;
}

void AspeedDecoder::decode_rgba_into(
    const AspeedDecodeOptions& options,
    const std::vector<std::uint8_t>& compressed,
    std::span<std::uint8_t> output_rgba)
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

    const std::size_t output_size =
        static_cast<std::size_t>(options.width) * static_cast<std::size_t>(options.height) * 4;
    if (output_rgba.size() != output_size) {
        throw std::invalid_argument("ASPEED output buffer size does not match dimensions");
    }

    std::vector<unsigned long> input_words((compressed.size() + 3) / 4 + 2);
    for (std::size_t i = 0; i < compressed.size(); ++i) {
        append_le_word(input_words, i / 4, compressed[i], static_cast<int>(i % 4));
    }

    const unsigned chroma_selector =
        options.use_separate_chroma_selectors ? options.chroma_table_selector : options.jpeg_table_selector;
    const unsigned advance_chroma_selector = options.use_separate_chroma_selectors
        ? options.advance_chroma_table_selector
        : options.advance_table_selector;

    std::lock_guard lock(g_aspeed_decode_mutex);
    decode_ext(
        input_words.data(),
        static_cast<int>(compressed.size()),
        output_rgba.data(),
        options.width,
        options.height,
        options.mode420,
        options.jpeg_table_selector,
        chroma_selector,
        options.advance_table_selector,
        advance_chroma_selector,
        options.yuv_table_mapping);

#ifdef HITSC_ASPEED_DEBUG
    {
        static const char* const kTermName[3] = {"FRAME_END", "RUNAWAY_GUARD", "BUFFER_END"};
        std::ostringstream line;
        line << "[aspeed] dest=" << options.width << "x" << options.height
             << " src=" << options.source_width << "x" << options.source_height
             << " mode420=" << options.mode420
             << " sel=" << options.jpeg_table_selector
             << " chroma=" << chroma_selector
             << " adv=" << options.advance_table_selector
             << " advchroma=" << advance_chroma_selector
             << " map=" << options.yuv_table_mapping
             << " bytes=" << compressed.size()
             << " term="
             << (g_aspeed_termination >= 0 && g_aspeed_termination <= 2
                     ? kTermName[g_aspeed_termination]
                     : "?")
             << " iters=" << g_aspeed_iterations
             << " last_mb=" << g_aspeed_final_mbx << "," << g_aspeed_final_mby
             << " bad_sel=" << g_aspeed_bad_selector << " codes=[";
        for (int i = 0; i < 16; ++i) {
            if (g_aspeed_block_code_counts[i] != 0) {
                line << i << ":" << g_aspeed_block_code_counts[i] << " ";
            }
        }
        line << "]";
        std::cerr << line.str() << std::endl;
        std::ofstream log_file("hitsc_aspeed_debug.log", std::ios::app);
        if (log_file) {
            log_file << line.str() << "\n";
        }
    }
#endif
}

} // namespace hitsc
