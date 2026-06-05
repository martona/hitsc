#pragma once

#include <cstdint>
#include <span>
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
    // 1 => dequantize chroma with the luminance tables instead of the chrominance
    // tables (ASPEED JPEGYUVTableMapping). MegaRAC sets this per frame; 0 elsewhere.
    unsigned yuv_table_mapping = 0;
    bool use_separate_chroma_selectors = false;
    // Host (Source) resolution from the frame header. Diagnostic for now: the
    // reference decoder strides output by Source while gridding macroblocks by
    // Destination (width/height above). 0 => unknown / same as Destination.
    int source_width = 0;
    int source_height = 0;
};

// Pixel-space bounding box of the macroblocks a decode actually rewrote, for a
// partial GPU texture upload. valid == false => no blocks were written this frame
// (a no-op delta); callers should treat that as "nothing changed".
struct AspeedDirtyRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool valid = false;
};

// Outcome of decoding one frame into the decoder's framebuffer.
struct AspeedDecodedDelta {
    bool ok = false;       // decode succeeded (false => skip this frame)
    bool full = false;     // whole frame is new (first frame / resolution change)
    AspeedDirtyRect rect;  // else: the region that changed (rect.valid == false => no-op)
};

// A stateful ASPEED differential-video decoder (ATEN + MegaRAC share this codec).
// ASPEED frames are deltas on the previous frame (SKIP/PASS2 blocks), so the decoder
// owns the persistent RGBA framebuffer and decode() updates it in place, reporting
// the changed bounding box. NOTE: the underlying C codec has process-global state
// behind a mutex, so decodes across instances serialize and share its table caches.
class AspeedDecoder {
public:
    AspeedDecoder();

    // Decode the next delta in place into the owned framebuffer.
    AspeedDecodedDelta decode(
        const AspeedDecodeOptions& options,
        const std::vector<std::uint8_t>& compressed,
        int width,
        int height);

    // The current decoded frame: RGBA8888, tightly packed (width*height*4 bytes).
    std::span<const std::uint8_t> framebuffer() const { return clean_rgba_; }
    int width() const { return clean_width_; }
    int height() const { return clean_height_; }

    // Drop the framebuffer so the next decode() re-seeds a full frame (reconnect).
    void reset();

private:
    // Stateless primitive: decode in place into a caller buffer, report the dirty rect.
    void decode_into(
        const AspeedDecodeOptions& options,
        const std::vector<std::uint8_t>& compressed,
        std::span<std::uint8_t> output_rgba,
        AspeedDirtyRect* dirty);

    std::vector<std::uint8_t> clean_rgba_;
    int clean_width_ = 0;
    int clean_height_ = 0;
};

} // namespace hitsc
