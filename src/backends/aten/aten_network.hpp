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
};

void run_aten_network_session(
    const AtenViewOptions& options,
    AtenViewState& state,
    std::atomic_bool& stop_requested);

} // namespace hitsc
