#pragma once

#include "backends/aspeed/aspeed_decoder.hpp"
#include "backends/aspeed/aspeed_view_state.hpp"
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

struct MegaracInputWork {
    std::uint16_t type = 0;
    std::vector<std::uint8_t> packet;
};

struct MegaracViewSessionState : AspeedViewState {
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
