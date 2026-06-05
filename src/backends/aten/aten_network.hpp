#pragma once

#include "backends/aspeed/aspeed_view_state.hpp"

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

struct AtenViewState : AspeedViewState {
    InputQueue<std::vector<std::uint8_t>> input;
    // Host resolution from the RFB ServerInit, for pointer mapping before any video
    // frame decodes (wake-from-sleep). 0 => unknown. Set once by the network worker.
    std::atomic_int host_input_width{0};
    std::atomic_int host_input_height{0};
};

void run_aten_network_session(
    const AtenViewOptions& options,
    AtenViewState& state,
    std::atomic_bool& stop_requested);

} // namespace hitsc
