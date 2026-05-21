#pragma once

#include "aten_protocol.hpp"
#include "hardware_cursor.hpp"
#include "options.hpp"
#include "view_base.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace hitsc {

struct AtenCompressedFrame {
    int width = 0;
    int height = 0;
    std::uint64_t sequence = 0;
    int update_number = 0;
    AspeedDecodeOptions decode_options;
    std::vector<std::uint8_t> compressed;
    std::chrono::steady_clock::time_point received_at;
    std::chrono::steady_clock::time_point published_at;
    std::size_t websocket_bytes = 0;
};

struct AtenViewState : ViewStateBase {
    LatestMailbox<AtenCompressedFrame> frames;
    LatestMailbox<HardwareCursor> cursors;
    InputQueue<std::vector<std::uint8_t>> input;
};

void run_aten_network_session(
    const AtenViewOptions& options,
    AtenViewState& state,
    std::atomic_bool& stop_requested);

} // namespace hitsc
