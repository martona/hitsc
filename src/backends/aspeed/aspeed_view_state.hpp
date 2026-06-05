#pragma once

#include "backends/aspeed/aspeed_decoder.hpp"  // AspeedDecodeOptions
#include "hardware_cursor.hpp"                   // HardwareCursor
#include "view_base.hpp"                          // ViewStateBase, FrameQueue, LatestMailbox

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hitsc {

// One queued ASPEED video frame. ATEN and MegaRAC share this differential codec;
// the decode pipeline reads only these fields (backend-specific parse metadata
// stays in the network layer and never reaches the queue). width/height are the
// Destination dimensions the decoder grids macroblocks by.
struct AspeedCompressedFrame {
    int width = 0;
    int height = 0;
    std::uint64_t sequence = 0;  // assigned by FrameQueue::publish
    AspeedDecodeOptions decode_options;
    std::vector<std::uint8_t> compressed;
    std::chrono::steady_clock::time_point received_at;
    std::chrono::steady_clock::time_point published_at;
    std::size_t websocket_bytes = 0;
};

// Shared session-state core for the software-decoded ASPEED backends. Each backend's
// session state derives from this and adds its own (typed) input queue and extras.
// The hardware cursor is the same HardwareCursor type for both backends.
struct AspeedViewState : ViewStateBase {
    FrameQueue<AspeedCompressedFrame> frames;  // differential stream: never drop
    LatestMailbox<HardwareCursor> cursors;
};

} // namespace hitsc
