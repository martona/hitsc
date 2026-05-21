#pragma once

#include "aten_protocol.hpp"
#include "hardware_cursor.hpp"
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

struct AtenViewState {
    std::mutex frame_mutex;
    std::mutex control_mutex;
    std::shared_ptr<const AtenCompressedFrame> frame;
    HardwareCursor cursor;
    std::atomic_uint32_t frame_event_type{0};
    std::atomic_bool frame_event_pending{false};
    ViewStatus view_status;
    std::exception_ptr exception;
    std::function<void()> force_close;
    std::function<void(std::vector<std::uint8_t>)> input_sink;
    std::deque<std::vector<std::uint8_t>> pending_input;
    std::uint64_t frame_sequence = 0;
    std::uint64_t cursor_sequence = 0;
    bool has_cursor = false;
};

void run_aten_network_session(
    const AtenViewOptions& options,
    AtenViewState& state,
    std::atomic_bool& stop_requested);

void stop_aten_network(
    AtenViewState& state,
    std::atomic_bool& stop_requested,
    std::thread& network_thread);

void set_aten_exception(AtenViewState& state, std::exception_ptr exception);
std::exception_ptr take_aten_exception(AtenViewState& state);

std::shared_ptr<const AtenCompressedFrame> take_latest_aten_frame(
    AtenViewState& state,
    std::uint64_t last_sequence);
void clear_latest_aten_frame(AtenViewState& state);

std::optional<HardwareCursor> take_latest_aten_cursor(
    AtenViewState& state,
    std::uint64_t last_sequence);

void queue_aten_input_packet(
    AtenViewState& state,
    std::vector<std::uint8_t> packet);

void queue_aten_key_event(
    AtenViewState& state,
    std::uint32_t usage,
    bool down);
    
void queue_aten_pointer_event(
    AtenViewState& state,
    int x,
    int y,
    std::uint8_t mask);

} // namespace hitsc
