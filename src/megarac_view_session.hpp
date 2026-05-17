#pragma once

#include "megarac_cursor.hpp"
#include "megarac_hid.hpp"
#include "options.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace hitsc {

struct MegaracViewFrame {
    int width = 0;
    int height = 0;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> rgba;
};

struct MegaracViewSessionState {
    std::mutex frame_mutex;
    std::mutex cursor_mutex;
    std::mutex control_mutex;
    MegaracViewFrame frame;
    MegaracHardwareCursor cursor;
    std::string status = "starting";
    std::string subprotocol;
    std::function<void(std::uint16_t, std::vector<std::uint8_t>, bool)> send_packet;
    std::function<void()> stop_network;
    std::function<void()> force_close;
    bool has_frame = false;
    bool has_cursor = false;
    int mouse_mode = kMegaracAbsoluteMouseMode;
};

struct MegaracViewStatusSnapshot {
    std::string status;
    bool has_frame = false;
};

std::mutex& log_mutex();

template <typename Writer>
void write_log_line(std::ostream& output, Writer writer)
{
    std::lock_guard lock(log_mutex());
    writer(output);
    output << '\n';
}

void run_megarac_view_session(
    const MegaracViewOptions& options,
    MegaracViewSessionState& state,
    const std::atomic_bool& stop_requested);

void stop_megarac_view_session(MegaracViewSessionState& state);

bool queue_megarac_view_packet(
    MegaracViewSessionState& state,
    std::uint16_t type,
    std::vector<std::uint8_t> packet,
    bool coalesce);

int megarac_view_mouse_mode_snapshot(MegaracViewSessionState& state);

std::optional<MegaracViewFrame> take_latest_megarac_view_frame(
    MegaracViewSessionState& state,
    std::uint64_t last_sequence);

std::optional<MegaracHardwareCursor> take_latest_megarac_view_cursor(
    MegaracViewSessionState& state,
    std::uint64_t last_sequence);

MegaracViewStatusSnapshot megarac_view_status_snapshot(MegaracViewSessionState& state);

int sampled_average_rgb(const std::vector<std::uint8_t>& rgba);

} // namespace hitsc
