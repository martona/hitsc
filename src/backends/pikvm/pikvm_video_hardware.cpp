#include "pikvm_video_hardware.hpp"

#include "log.hpp"

extern "C" {
#include <libavcodec/codec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>

#include <d3d11_4.h>  // ID3D11Multithread (pulls in d3d11.h)
#include <dxgi.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

#include <cwchar>
#endif

namespace hitsc {
namespace {

#ifdef _WIN32

using Microsoft::WRL::ComPtr;

std::string ffmpeg_error_string(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

std::runtime_error ffmpeg_error(int code, const char* context)
{
    return std::runtime_error(std::string(context) + ": " + ffmpeg_error_string(code));
}

void d3d11_lock_callback(void* lock_context)
{
    static_cast<std::recursive_mutex*>(lock_context)->lock();
}

void d3d11_unlock_callback(void* lock_context)
{
    static_cast<std::recursive_mutex*>(lock_context)->unlock();
}

std::string wide_to_utf8(const wchar_t* value)
{
    if (value == nullptr || value[0] == L'\0') {
        return {};
    }
    const int wide_length = static_cast<int>(std::wcslen(value));
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, value, wide_length, nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, wide_length, result.data(), length, nullptr, nullptr);
    return result;
}

bool d3d11_device_is_software_adapter(ID3D11Device* device, std::string& description)
{
    if (device == nullptr) {
        return false;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(&adapter))) {
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter1;
    if (FAILED(adapter.As(&adapter1))) {
        return false;
    }
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter1->GetDesc1(&desc))) {
        return false;
    }
    description = wide_to_utf8(desc.Description);
    return (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
}

AVFrame* hardware_frame_owner(const PikvmVideoFrame& frame)
{
    auto* owner = static_cast<AVFrame*>(frame.owner.get());
    if (owner == nullptr) {
        throw std::runtime_error("hardware PiKVM frame is missing its owner");
    }
    return owner;
}

class PikvmD3D11VideoHardware final : public PikvmVideoHardware {
public:
    explicit PikvmD3D11VideoHardware(ID3D11Device* device)
        : device_(device)
        , lock_(std::make_shared<std::recursive_mutex>())
    {
        if (device_ == nullptr) {
            throw std::runtime_error("D3D11 hardware video requires a device");
        }
        device_->AddRef();  // QRhi owns it; keep it alive for the decoder's lifetime
    }

    ~PikvmD3D11VideoHardware() override
    {
        device_->Release();
    }

    const char* name() const override
    {
        return "d3d11";
    }

    AVPixelFormat pixel_format() const override
    {
        return AV_PIX_FMT_D3D11;
    }

    bool codec_supported(const AVCodec* codec) const override
    {
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
            if (config == nullptr) {
                return false;
            }
            if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA
                && config->pix_fmt == AV_PIX_FMT_D3D11
                && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
                return true;
            }
        }
    }

    AVBufferRef* create_device_context() const override
    {
        AVBufferRef* device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (device_ref == nullptr) {
            throw std::runtime_error("failed to allocate FFmpeg D3D11 device context");
        }

        auto* device_context = reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
        auto* d3d11 = reinterpret_cast<AVD3D11VADeviceContext*>(device_context->hwctx);
        d3d11->device = device_;
        d3d11->device->AddRef();
        d3d11->lock = &d3d11_lock_callback;
        d3d11->unlock = &d3d11_unlock_callback;
        d3d11->lock_ctx = lock_.get();
        d3d11->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;

        const int result = av_hwdevice_ctx_init(device_ref);
        if (result < 0) {
            av_buffer_unref(&device_ref);
            throw ffmpeg_error(result, "failed to initialize FFmpeg D3D11 device context");
        }
        return device_ref;
    }

    PikvmVideoFrame reference_frame(const AVFrame& frame) const override
    {
        auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame.data[0]);
        if (texture == nullptr) {
            throw std::runtime_error("decoded hardware H.264 frame is missing its texture");
        }

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_NV12) {
            throw std::runtime_error("decoded hardware H.264 frame is not NV12");
        }

        AVFrame* cloned = av_frame_clone(&frame);
        if (cloned == nullptr) {
            throw std::runtime_error("failed to reference decoded hardware FFmpeg frame");
        }

        PikvmVideoFrame output;
        output.width = cloned->width;
        output.height = cloned->height;
        output.format = PikvmVideoPixelFormat::hardware_nv12;
        output.owner = std::shared_ptr<void>(cloned, [](void* pointer) {
            AVFrame* owned_frame = static_cast<AVFrame*>(pointer);
            av_frame_free(&owned_frame);
        });
        return output;
    }

    std::unique_lock<std::recursive_mutex> lock() const override
    {
        return std::unique_lock<std::recursive_mutex>(*lock_);
    }

    std::shared_ptr<std::recursive_mutex> lock_handle() const override
    {
        return lock_;
    }

    ID3D11Texture2D* frame_texture(const PikvmVideoFrame& frame) const override
    {
        AVFrame* owner = hardware_frame_owner(frame);
        auto* texture = reinterpret_cast<ID3D11Texture2D*>(owner->data[0]);
        if (texture == nullptr) {
            throw std::runtime_error("hardware PiKVM frame is missing its source texture");
        }
        return texture;
    }

    int frame_array_slice(const PikvmVideoFrame& frame) const override
    {
        AVFrame* owner = hardware_frame_owner(frame);
        return static_cast<int>(reinterpret_cast<intptr_t>(owner->data[1]));
    }

private:
    ID3D11Device* device_ = nullptr;
    std::shared_ptr<std::recursive_mutex> lock_;
};

#endif  // _WIN32

} // namespace

std::shared_ptr<PikvmVideoHardware> make_pikvm_d3d11_hardware(ID3D11Device* device, bool verbose)
{
#ifdef _WIN32
    if (device == nullptr) {
        return {};
    }

    std::string adapter_description;
    if (d3d11_device_is_software_adapter(device, adapter_description)) {
        if (verbose) {
            log_warning() << "hardware video disabled for software adapter"
                          << (adapter_description.empty() ? "" : ": ") << adapter_description;
        }
        return {};
    }

    // The decoder runs on the network thread; QRhi renders on the GUI thread. Both
    // touch this device, so multithread protection is required (alongside the
    // render-frame lock).
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    ComPtr<ID3D11Multithread> multithread;
    if (context && SUCCEEDED(context.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    auto hardware = std::make_shared<PikvmD3D11VideoHardware>(device);
    if (verbose) {
        log_info() << "pikvm hardware video enabled"
                   << " adapter=\"" << adapter_description << "\"";
    }
    return hardware;
#else
    (void)device;
    (void)verbose;
    return {};
#endif
}

} // namespace hitsc
