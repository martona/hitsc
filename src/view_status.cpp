#include "view_status.hpp"

#include <cmath>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hitsc {
namespace {

// Total CPU time (kernel + user) this process has consumed since launch, in
// seconds; -1 if unavailable. Sampled once per title-refresh window to derive a %.
double process_cpu_seconds()
{
#ifdef _WIN32
    FILETIME creation, exit_time, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user) == 0) {
        return -1.0;
    }
    const auto to_u64 = [](const FILETIME& ft) {
        return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    return static_cast<double>(to_u64(kernel) + to_u64(user)) * 1e-7; // 100-ns ticks -> seconds
#else
    return -1.0;
#endif
}

// Total logical processors across ALL processor groups, cached. Must use
// GetActiveProcessorCount(ALL_PROCESSOR_GROUPS): on >64-thread machines (e.g. a
// 96-core / 192-thread Threadripper) Windows splits CPUs into processor groups,
// and the legacy GetSystemInfo / std::thread::hardware_concurrency only report the
// current 64-CPU group -- which would make the CPU% read several times too high.
unsigned logical_processor_count()
{
#ifdef _WIN32
    static const unsigned count = []() -> unsigned {
        const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        return n > 0 ? static_cast<unsigned>(n) : 1u;
    }();
    return count;
#else
    return 1u; // CPU% is only computed on Windows (process_cpu_seconds() returns -1 elsewhere)
#endif
}

} // namespace

void ViewStatus::data_received(std::size_t bytes)
{
    std::lock_guard lock(mutex_);
    bucket_bytes_ += bytes;
}

void ViewStatus::frame_presented(int width, int height)
{
    std::lock_guard lock(mutex_);
    width_ = width;
    height_ = height;
    frame_ready_ = true;
    ++bucket_frames_;
}

void ViewStatus::kvm_display_status(bool online)
{
    std::lock_guard lock(mutex_);
    display_online_ = online;
    if (!online) {
        frame_ready_ = false;
    }
}

void ViewStatus::kvm_connection(bool connected)
{
    std::lock_guard lock(mutex_);
    connected_ = connected;
    if (!connected_) {
        display_online_.reset();
        frame_ready_ = false;
        width_ = 0;
        height_ = 0;
        bucket_started_at_ = Clock::now();
        bucket_bytes_ = 0;
        bucket_frames_ = 0;
        kbps_ = 0.0;
        fps_ = 0.0;
        cpu_percent_ = 0.0;
        last_cpu_seconds_ = -1.0; // re-baseline so the first post-reconnect % isn't measured over the gap
    }
}

void ViewStatus::minimize()
{
    std::lock_guard lock(mutex_);
    frame_ready_ = false;
    bucket_frames_ = 0;
    fps_ = 0.0;
}

ViewRenderState ViewStatus::render_state()
{
    std::lock_guard lock(mutex_);
    return ViewRenderState{connected_, display_online_, frame_ready_};
}

std::string ViewStatus::title(std::string_view hostname)
{
    std::lock_guard lock(mutex_);
    update_rates(Clock::now());

    std::ostringstream out;
    out << hostname
        << " | " << dimensions_text()
        << " | " << bandwidth_text()
        << " | " << fps_text()
        << " | " << cpu_text()
        << " | " << state_text();
    return out.str();
}

void ViewStatus::update_rates(Clock::time_point now)
{
    const auto elapsed = now - bucket_started_at_;
    if (elapsed < std::chrono::seconds(1)) {
        return;
    }

    const double seconds = std::chrono::duration<double>(elapsed).count();
    if (seconds > 0.0) {
        kbps_ = (static_cast<double>(bucket_bytes_) * 8.0) / 1000.0 / seconds;
        fps_ = static_cast<double>(bucket_frames_) / seconds;

        // CPU this process burned over the window, as a percentage of the WHOLE
        // machine (all logical processors across all groups) -- 100% == every thread
        // maxed -- to match Task Manager / System Informer. One pegged core therefore
        // reads ~100/threads % (e.g. ~0.5% on a 192-thread box).
        const double cpu_now = process_cpu_seconds();
        if (cpu_now >= 0.0 && last_cpu_seconds_ >= 0.0) {
            const double cpu_delta = cpu_now - last_cpu_seconds_;
            cpu_percent_ = cpu_delta > 0.0
                ? (cpu_delta / seconds) * 100.0 / logical_processor_count()
                : 0.0;
        }
        last_cpu_seconds_ = cpu_now;
    }

    bucket_started_at_ = now;
    bucket_bytes_ = 0;
    bucket_frames_ = 0;
}

std::string ViewStatus::dimensions_text() const
{
    if (width_ <= 0 || height_ <= 0) {
        return "- x -";
    }

    return std::to_string(width_) + " x " + std::to_string(height_);
}

std::string ViewStatus::bandwidth_text() const
{
    return std::to_string(static_cast<long long>(std::llround(kbps_))) + " kbps";
}

std::string ViewStatus::fps_text() const
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << fps_ << " fps";
    return out.str();
}

std::string ViewStatus::cpu_text() const
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << cpu_percent_ << "% cpu";
    return out.str();
}

std::string ViewStatus::state_text() const
{
    if (!connected_ || !display_online_.has_value()) {
        return "No connection";
    }

    if (!*display_online_) {
        return "Connected, no input";
    }

    if (!frame_ready_) {
        return "Connected, waiting for video";
    }

    return "Showing video";
}

} // namespace hitsc
