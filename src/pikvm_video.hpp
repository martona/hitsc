#pragma once

#include "options.hpp"
#include "pikvm_events.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace hitsc {

enum class PikvmVideoPixelFormat {
    rgba32,
    i420,
    nv12,
};

struct PikvmVideoFrame {
    int width = 0;
    int height = 0;
    std::uint64_t sequence = 0;
    PikvmVideoPixelFormat format = PikvmVideoPixelFormat::rgba32;
    std::array<const std::uint8_t*, 4> planes{};
    std::array<int, 4> pitches{};
    std::shared_ptr<void> owner;
    std::vector<std::uint8_t> rgba;
};

const char* pikvm_video_pixel_format_name(PikvmVideoPixelFormat format);
std::size_t pikvm_video_frame_payload_bytes(const PikvmVideoFrame& frame);

class PikvmH264Decoder {
public:
    PikvmH264Decoder();
    ~PikvmH264Decoder();

    PikvmH264Decoder(const PikvmH264Decoder&) = delete;
    PikvmH264Decoder& operator=(const PikvmH264Decoder&) = delete;

    void set_verbose(bool verbose);
    std::vector<PikvmVideoFrame> ingest(std::span<const std::uint8_t> bytes);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void start_pikvm_video_stream(
    std::shared_ptr<PikvmWebSocket> ws,
    PikvmViewOptions options,
    const std::atomic_bool& stop_requested,
    std::function<void(PikvmVideoFrame)> on_frame,
    std::function<void(std::exception_ptr)> on_error);

} // namespace hitsc
