#pragma once

#include "options.hpp"
#include "pikvm_events.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <vector>

namespace hitsc {

struct PikvmVideoFrame {
    int width = 0;
    int height = 0;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> rgba;
};

class PikvmH264Decoder {
public:
    PikvmH264Decoder();
    ~PikvmH264Decoder();

    PikvmH264Decoder(const PikvmH264Decoder&) = delete;
    PikvmH264Decoder& operator=(const PikvmH264Decoder&) = delete;

    void set_verbose(bool verbose);
    std::vector<PikvmVideoFrame> ingest(const std::vector<std::uint8_t>& bytes);

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
