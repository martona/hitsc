#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace hitsc {

class ViewStatus {
public:
    void data_received(std::size_t bytes);
    void frame_presented(int width, int height);
    void kvm_display_status(bool online);
    void kvm_connection(bool connected);
    void minimize();

    std::string title(std::string_view hostname);

private:
    using Clock = std::chrono::steady_clock;

    void update_rates(Clock::time_point now);
    std::string dimensions_text() const;
    std::string bandwidth_text() const;
    std::string fps_text() const;
    std::string state_text() const;

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
};

} // namespace hitsc
