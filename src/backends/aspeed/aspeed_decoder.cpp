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

// Always present: the wrapper reads these in release to drive partial uploads.
extern int g_aspeed_dirty_x0;
extern int g_aspeed_dirty_y0;
extern int g_aspeed_dirty_x1;
extern int g_aspeed_dirty_y1;

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

AspeedDecodedDelta AspeedDecoder::decode(
    const AspeedDecodeOptions& options,
    const std::vector<std::uint8_t>& compressed,
    int width,
    int height)
{
    AspeedDecodedDelta delta;
    if (width <= 0 || height <= 0) {
        return delta;  // ok stays false
    }
    const std::size_t size =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    const bool reuse =
        clean_width_ == width && clean_height_ == height && clean_rgba_.size() == size;
    if (!reuse) {
        // First frame / resolution change: seed white (so SKIP blocks have something
        // to keep) and force a full upload.
        clean_rgba_.assign(size, 0xff);
        delta.full = true;
    }
    AspeedDirtyRect rect;
    try {
        // decode_into validates before writing, so a bad frame throws without
        // half-updating the framebuffer (on reuse the prior frame is preserved).
        decode_into(options, compressed, clean_rgba_, &rect);
    } catch (...) {
        return delta;  // ok stays false
    }
    clean_width_ = width;
    clean_height_ = height;
    delta.ok = true;
    if (!delta.full) {
        delta.rect = rect;
    }
    return delta;
}

void AspeedDecoder::reset()
{
    clean_rgba_.clear();
    clean_width_ = 0;
    clean_height_ = 0;
}

void AspeedDecoder::decode_into(
    const AspeedDecodeOptions& options,
    const std::vector<std::uint8_t>& compressed,
    std::span<std::uint8_t> output_rgba,
    AspeedDirtyRect* dirty)
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

    // Still holding g_aspeed_decode_mutex: read the dirty bbox the decoder just
    // accumulated before another decode can clobber the globals.
    if (dirty != nullptr) {
        if (g_aspeed_dirty_x1 > g_aspeed_dirty_x0 && g_aspeed_dirty_y1 > g_aspeed_dirty_y0) {
            *dirty = AspeedDirtyRect{
                g_aspeed_dirty_x0,
                g_aspeed_dirty_y0,
                g_aspeed_dirty_x1 - g_aspeed_dirty_x0,
                g_aspeed_dirty_y1 - g_aspeed_dirty_y0,
                true,
            };
        } else {
            *dirty = AspeedDirtyRect{};
        }
    }

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
