#pragma once

#include "backends/aspeed/aspeed_decoder.hpp"
#include "hardware_cursor.hpp"
#include "megarac_hid.hpp"
#include "options.hpp"
#include "view_base.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace hitsc {

struct MegaracCompressedFrame {
    int width = 0;
    int height = 0;
    std::uint64_t sequence = 0;
    int frame_number = 0;
    std::uint8_t compression_mode = 0;
    std::uint8_t first_block_header = 0;
    AspeedDecodeOptions decode_options;
    std::vector<std::uint8_t> compressed;
    std::chrono::steady_clock::time_point received_at;
    std::chrono::steady_clock::time_point published_at;
    std::size_t websocket_bytes = 0;
};

struct MegaracInputWork {
    std::uint16_t type = 0;
    std::vector<std::uint8_t> packet;
};

struct MegaracViewSessionState : ViewStateBase {
    LatestMailbox<MegaracCompressedFrame> frames;
    LatestMailbox<MegaracHardwareCursor> cursors;
    InputQueue<MegaracInputWork> input;
    std::atomic_int mouse_mode{kMegaracAbsoluteMouseMode};
    std::atomic_uint64_t video_feedback_presented_frames{0};
};

void run_megarac_view_session(
    const MegaracViewOptions& options,
    MegaracViewSessionState& state,
    const std::atomic_bool& stop_requested);

int megarac_view_mouse_mode_snapshot(MegaracViewSessionState& state);

} // namespace hitsc
