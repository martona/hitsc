#pragma once

#include "aspeed_decoder.hpp"
#include "megarac_cursor.hpp"
#include "megarac_hid.hpp"
#include "options.hpp"
#include "view_status.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
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

struct PendingMegaracInputPacket {
    std::uint16_t type = 0;
    std::vector<std::uint8_t> packet;
    bool coalesce = false;
};

struct MegaracViewSessionState {
    std::mutex frame_mutex;
    std::mutex control_mutex;
    std::shared_ptr<const MegaracCompressedFrame> frame;
    MegaracHardwareCursor cursor;
    std::atomic_uint32_t frame_event_type{0};
    std::atomic_bool frame_event_pending{false};
    std::atomic_int mouse_mode{kMegaracAbsoluteMouseMode};
    std::atomic_uint64_t frames_presented{0};
    ViewStatus view_status;
    std::exception_ptr exception;
    std::function<void()> force_close;
    std::function<void(std::uint16_t, std::vector<std::uint8_t>, bool)> input_sink;
    std::deque<PendingMegaracInputPacket> pending_input;
    std::uint64_t frame_sequence = 0;
    std::uint64_t cursor_sequence = 0;
    bool has_cursor = false;
};

void run_megarac_view_session(
    const MegaracViewOptions& options,
    MegaracViewSessionState& state,
    const std::atomic_bool& stop_requested);

void stop_megarac_network(
    MegaracViewSessionState& state,
    std::atomic_bool& stop_requested,
    std::thread& network_thread,
    bool verbose);

void set_megarac_exception(MegaracViewSessionState& state, std::exception_ptr exception);
std::exception_ptr take_megarac_exception(MegaracViewSessionState& state);

bool queue_megarac_view_packet(
    MegaracViewSessionState& state,
    std::uint16_t type,
    std::vector<std::uint8_t> packet,
    bool coalesce);

int megarac_view_mouse_mode_snapshot(MegaracViewSessionState& state);

std::shared_ptr<const MegaracCompressedFrame> take_latest_megarac_view_frame(
    MegaracViewSessionState& state,
    std::uint64_t last_sequence);
void clear_latest_megarac_view_frame(MegaracViewSessionState& state);

std::optional<MegaracHardwareCursor> take_latest_megarac_view_cursor(
    MegaracViewSessionState& state,
    std::uint64_t last_sequence);

} // namespace hitsc
