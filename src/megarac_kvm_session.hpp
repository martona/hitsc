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

struct MegaracKvmFrame {
    int width = 0;
    int height = 0;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> rgba;
};

struct MegaracKvmSessionState {
    std::mutex frame_mutex;
    std::mutex cursor_mutex;
    std::mutex control_mutex;
    MegaracKvmFrame frame;
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

struct MegaracKvmStatusSnapshot {
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

void run_megarac_kvm_session(
    const KvmViewOptions& options,
    MegaracKvmSessionState& state,
    const std::atomic_bool& stop_requested);

void stop_megarac_kvm_session(MegaracKvmSessionState& state);

bool queue_megarac_kvm_packet(
    MegaracKvmSessionState& state,
    std::uint16_t type,
    std::vector<std::uint8_t> packet,
    bool coalesce);

int megarac_kvm_mouse_mode_snapshot(MegaracKvmSessionState& state);

std::optional<MegaracKvmFrame> take_latest_megarac_kvm_frame(
    MegaracKvmSessionState& state,
    std::uint64_t last_sequence);

std::optional<MegaracHardwareCursor> take_latest_megarac_kvm_cursor(
    MegaracKvmSessionState& state,
    std::uint64_t last_sequence);

MegaracKvmStatusSnapshot megarac_kvm_status_snapshot(MegaracKvmSessionState& state);

int sampled_average_rgb(const std::vector<std::uint8_t>& rgba);

} // namespace hitsc
