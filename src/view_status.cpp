#include "view_status.hpp"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace hitsc {

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
    }
}

void ViewStatus::minimize()
{
    std::lock_guard lock(mutex_);
    frame_ready_ = false;
    bucket_frames_ = 0;
    fps_ = 0.0;
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
