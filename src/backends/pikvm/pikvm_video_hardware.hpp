#pragma once

#include "pikvm_video.hpp"

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <memory>
#include <mutex>

struct AVBufferRef;
struct AVCodec;
struct AVFrame;
struct ID3D11Device;
struct ID3D11Texture2D;

namespace hitsc {

// Hardware H.264 decode bound to an EXISTING ID3D11Device (QRhi's), so decoded
// NV12 textures live on the same device QRhi renders with and can be imported
// zero-copy. One recursive_mutex is shared as FFmpeg's D3D11VA lock callback AND
// the renderer's lock (held across the render frame) -- both are required to keep
// the decoder's device work from racing the renderer's (see [[hitsc-viewer-threading]]).
class PikvmVideoHardware {
public:
    virtual ~PikvmVideoHardware() = default;

    virtual const char* name() const = 0;
    virtual AVPixelFormat pixel_format() const = 0;
    virtual bool codec_supported(const AVCodec* codec) const = 0;
    virtual AVBufferRef* create_device_context() const = 0;
    virtual PikvmVideoFrame reference_frame(const AVFrame& frame) const = 0;

    virtual std::unique_lock<std::recursive_mutex> lock() const = 0;
    virtual std::shared_ptr<std::recursive_mutex> lock_handle() const = 0;

    // The decoder's NV12 texture array + the slice index backing this frame.
    virtual ID3D11Texture2D* frame_texture(const PikvmVideoFrame& frame) const = 0;
    virtual int frame_array_slice(const PikvmVideoFrame& frame) const = 0;
};

// Bind D3D11VA hardware decode to `device` (QRhi's). Enables D3D11 multithread
// protection on it. Returns null when hardware decode isn't usable (non-Windows,
// null device, or a software/WARP adapter) so the caller falls back to software.
std::shared_ptr<PikvmVideoHardware> make_pikvm_d3d11_hardware(ID3D11Device* device, bool verbose);

} // namespace hitsc
