#pragma once

#include "pikvm_video.hpp"

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <memory>
#include <mutex>
#include <string>

struct AVBufferRef;
struct AVCodec;
struct AVFrame;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Window;

namespace hitsc {

class PikvmVideoHardware {
public:
    virtual ~PikvmVideoHardware() = default;

    virtual const char* name() const = 0;
    virtual AVPixelFormat pixel_format() const = 0;
    virtual bool codec_supported(const AVCodec* codec) const = 0;
    virtual AVBufferRef* create_device_context() const = 0;
    virtual PikvmVideoFrame reference_frame(const AVFrame& frame) const = 0;
    virtual std::unique_lock<std::recursive_mutex> lock() const = 0;

    virtual bool frame_can_wrap_direct(const PikvmVideoFrame& frame) const = 0;
    virtual const void* frame_source_id(const PikvmVideoFrame& frame) const = 0;
    virtual SDL_Texture* try_create_wrapped_texture(
        SDL_Renderer* renderer,
        const PikvmVideoFrame& frame,
        std::string& error) const = 0;
    virtual void copy_frame_to_texture(SDL_Texture* texture, const PikvmVideoFrame& frame) const = 0;
    virtual bool texture_can_receive_copy(SDL_Texture* texture) const = 0;
};

struct PikvmVideoHardwareRenderer {
    SDL_Renderer* renderer = nullptr;
    std::shared_ptr<PikvmVideoHardware> hardware;
};

PikvmVideoHardwareRenderer try_create_pikvm_video_hardware_renderer(
    SDL_Window* window,
    bool verbose);

} // namespace hitsc
