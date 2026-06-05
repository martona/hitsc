#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// Temporary hardware-cursor diagnostics (defined here so every TU that includes
// view_status.hpp -- the backends and the viewer host -- sees the same toggle).
//   HITSC_DEBUG_HW_CURSOR: append a "hwcur ..." field to the title bar showing the
//     BMC cursor-packet rate, how many carry real (non-uniform) sprite data, and
//     the last sprite's size/position/type. Tells you whether the hardware-cursor
//     path is actually live, vs the cursor being baked into the JPEG video.
//   HITSC_DEBUG_HW_CURSOR_HIDE: stop drawing the overlay quad, so you can see if a
//     cursor still appears on screen (=> it is baked into the video, not ours).
//#define HITSC_DEBUG_HW_CURSOR 1
//#define HITSC_DEBUG_HW_CURSOR_HIDE 1

namespace hitsc {

// Snapshot of the fields the viewer needs to decide whether to draw video or the
// on-window console (and which console state).
struct ViewRenderState {
    bool connected = false;
    std::optional<bool> display_online;
    bool frame_ready = false;
};

class ViewStatus {
public:
    void data_received(std::size_t bytes);
    void frame_presented(int width, int height);
    void kvm_display_status(bool online);
    void kvm_connection(bool connected);
    void minimize();

#ifdef HITSC_DEBUG_HW_CURSOR
    // One view tick's hardware-cursor activity: how many BMC cursor packets arrived
    // (mailbox-sequence delta), whether the rendered sprite had real content, and
    // the last cursor's geometry/type. See HITSC_DEBUG_HW_CURSOR.
    void debug_cursor_tick(
        unsigned packets, bool nonuniform_sprite, int width, int height, int x, int y, int type);
#endif

    ViewRenderState render_state();

    std::string title(std::string_view hostname);

private:
    using Clock = std::chrono::steady_clock;

    void update_rates(Clock::time_point now);
    std::string dimensions_text() const;
    std::string bandwidth_text() const;
    std::string fps_text() const;
    std::string cpu_text() const;
    std::string state_text() const;
#ifdef HITSC_DEBUG_HW_CURSOR
    std::string hw_cursor_text() const;
#endif

    std::mutex mutex_;
    bool connected_ = false;
    std::optional<bool> display_online_;
    bool frame_ready_ = false;
    int width_ = 0;
    int height_ = 0;

    Clock::time_point bucket_started_at_ = Clock::now();
    std::uint64_t bucket_bytes_ = 0;
    std::uint64_t bucket_frames_ = 0;
    double kbps_ = 0.0;
    double fps_ = 0.0;
    double cpu_percent_ = 0.0;       // this process's CPU use over the last window, % of the whole machine
    double last_cpu_seconds_ = -1.0; // previous process-CPU-time sample (-1 = none yet)

#ifdef HITSC_DEBUG_HW_CURSOR
    std::uint64_t bucket_cursor_packets_ = 0; // BMC cursor packets this window
    std::uint64_t bucket_cursor_valid_ = 0;   // ticks this window with a non-uniform sprite
    double cursor_pps_ = 0.0;                  // cursor packets / sec
    double cursor_valid_ps_ = 0.0;             // valid (non-uniform) sprites / sec
    bool cursor_ever_ = false;                 // any cursor packet ever seen this session
    int cursor_w_ = 0;
    int cursor_h_ = 0;
    int cursor_x_ = 0;
    int cursor_y_ = 0;
    int cursor_type_ = -1;
#endif
};

} // namespace hitsc
