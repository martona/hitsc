#include "pikvm_video_hardware.hpp"

#include "log.hpp"

#include <SDL3/SDL.h>

extern "C" {
#include <libavcodec/codec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

#include <cwchar>
#include <cstdint>
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
    const int length = WideCharToMultiByte(
        CP_UTF8,
        0,
        value,
        wide_length,
        nullptr,
        0,
        nullptr,
        nullptr);
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

SDL_Renderer* try_create_named_renderer(
    SDL_Window* window,
    const char* name,
    std::string& error)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    if (props == 0) {
        error = SDL_GetError();
        return nullptr;
    }

    if (!SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window)
        || !SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, name)) {
        error = SDL_GetError();
        SDL_DestroyProperties(props);
        return nullptr;
    }

    SDL_Renderer* renderer = SDL_CreateRendererWithProperties(props);
    if (renderer == nullptr) {
        error = SDL_GetError();
    }
    SDL_DestroyProperties(props);
    return renderer;
}

ID3D11Device* renderer_d3d11_device(SDL_Renderer* renderer)
{
    SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
    if (props == 0) {
        return nullptr;
    }

    return static_cast<ID3D11Device*>(
        SDL_GetPointerProperty(props, SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr));
}

AVFrame* hardware_frame_owner(const PikvmVideoFrame& frame)
{
    auto* owner = static_cast<AVFrame*>(frame.owner.get());
    if (owner == nullptr) {
        throw std::runtime_error("hardware PiKVM frame is missing its owner");
    }
    return owner;
}

ID3D11Texture2D* hardware_frame_texture(const PikvmVideoFrame& frame)
{
    AVFrame* owner = hardware_frame_owner(frame);
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(owner->data[0]);
    if (texture == nullptr) {
        throw std::runtime_error("hardware PiKVM frame is missing its source texture");
    }
    return texture;
}

int hardware_frame_array_slice(const PikvmVideoFrame& frame)
{
    AVFrame* owner = hardware_frame_owner(frame);
    return static_cast<int>(reinterpret_cast<intptr_t>(owner->data[1]));
}

ID3D11Texture2D* sdl_texture_d3d11_resource(SDL_Texture* texture)
{
    SDL_PropertiesID props = SDL_GetTextureProperties(texture);
    if (props == 0) {
        return nullptr;
    }
    return static_cast<ID3D11Texture2D*>(
        SDL_GetPointerProperty(props, SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER, nullptr));
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

        AVHWDeviceContext* device_context =
            reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
        auto* d3d11 =
            reinterpret_cast<AVD3D11VADeviceContext*>(device_context->hwctx);
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

    bool frame_can_wrap_direct(const PikvmVideoFrame& frame) const override
    {
        if (frame.format != PikvmVideoPixelFormat::hardware_nv12) {
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        hardware_frame_texture(frame)->GetDesc(&desc);
        return desc.ArraySize == 1
            && hardware_frame_array_slice(frame) == 0
            && desc.Format == DXGI_FORMAT_NV12
            && (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
    }

    const void* frame_source_id(const PikvmVideoFrame& frame) const override
    {
        return hardware_frame_texture(frame);
    }

    SDL_Texture* try_create_wrapped_texture(
        SDL_Renderer* renderer,
        const PikvmVideoFrame& frame,
        std::string& error) const override
    {
        SDL_PropertiesID props = SDL_CreateProperties();
        if (props == 0) {
            error = SDL_GetError();
            return nullptr;
        }

        const bool ok =
            SDL_SetNumberProperty(
                props,
                SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                static_cast<Sint64>(SDL_PIXELFORMAT_NV12))
            && SDL_SetNumberProperty(
                props,
                SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                static_cast<Sint64>(SDL_TEXTUREACCESS_STATIC))
            && SDL_SetNumberProperty(
                props,
                SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,
                frame.width)
            && SDL_SetNumberProperty(
                props,
                SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,
                frame.height)
            && SDL_SetPointerProperty(
                props,
                SDL_PROP_TEXTURE_CREATE_D3D11_TEXTURE_POINTER,
                hardware_frame_texture(frame));
        if (!ok) {
            error = SDL_GetError();
            SDL_DestroyProperties(props);
            return nullptr;
        }

        SDL_Texture* texture = SDL_CreateTextureWithProperties(renderer, props);
        if (texture == nullptr) {
            error = SDL_GetError();
        }
        SDL_DestroyProperties(props);
        return texture;
    }

    void copy_frame_to_texture(SDL_Texture* texture, const PikvmVideoFrame& frame) const override
    {
        ID3D11Texture2D* source = hardware_frame_texture(frame);
        ID3D11Texture2D* destination = sdl_texture_d3d11_resource(texture);
        if (destination == nullptr) {
            throw std::runtime_error("SDL texture did not expose a hardware video texture");
        }

        D3D11_TEXTURE2D_DESC source_desc{};
        D3D11_TEXTURE2D_DESC destination_desc{};
        source->GetDesc(&source_desc);
        destination->GetDesc(&destination_desc);
        if (source_desc.Format != DXGI_FORMAT_NV12 || destination_desc.Format != DXGI_FORMAT_NV12) {
            throw std::runtime_error("hardware PiKVM texture copy requires NV12 textures");
        }

        const int array_slice = hardware_frame_array_slice(frame);
        if (array_slice < 0 || static_cast<UINT>(array_slice) >= source_desc.ArraySize) {
            throw std::runtime_error("hardware PiKVM frame has an invalid texture array slice");
        }

        const UINT source_subresource = D3D11CalcSubresource(
            0,
            static_cast<UINT>(array_slice),
            source_desc.MipLevels);

        std::lock_guard guard(*lock_);
        ComPtr<ID3D11DeviceContext> immediate_context;
        device_->GetImmediateContext(&immediate_context);
        if (!immediate_context) {
            throw std::runtime_error("failed to get D3D11 immediate context");
        }
        immediate_context->CopySubresourceRegion(
            destination,
            0,
            0,
            0,
            0,
            source,
            source_subresource,
            nullptr);
    }

    bool texture_can_receive_copy(SDL_Texture* texture) const override
    {
        return sdl_texture_d3d11_resource(texture) != nullptr;
    }

private:
    ID3D11Device* device_ = nullptr;
    std::shared_ptr<std::recursive_mutex> lock_;
};

#endif

} // namespace

PikvmVideoHardwareRenderer try_create_pikvm_video_hardware_renderer(
    SDL_Window* window,
    bool verbose)
{
#ifdef _WIN32
    std::string renderer_error;
    SDL_Renderer* renderer = try_create_named_renderer(window, "direct3d11", renderer_error);
    if (renderer == nullptr) {
        if (verbose) {
            log_warning() << "failed to create SDL hardware video renderer; using default renderer: "
                          << renderer_error;
        }
        return {};
    }

    ID3D11Device* device = renderer_d3d11_device(renderer);
    std::string adapter_description;
    const bool software_adapter =
        device != nullptr && d3d11_device_is_software_adapter(device, adapter_description);
    if (device == nullptr || software_adapter) {
        SDL_DestroyRenderer(renderer);
        if (verbose) {
            if (software_adapter) {
                log_warning() << "hardware video disabled for software adapter"
                              << (adapter_description.empty() ? "" : ": ")
                              << adapter_description;
            } else {
                log_warning() << "hardware video disabled; renderer did not expose a device";
            }
        }
        return {};
    }

    auto hardware = std::make_shared<PikvmD3D11VideoHardware>(device);
    if (verbose) {
        log_info() << "SDL renderer selected for PiKVM"
                   << " name=" << SDL_GetRendererName(renderer)
                   << " hardware=" << hardware->name()
                   << " adapter=\"" << adapter_description << "\"";
    }
    return {renderer, std::move(hardware)};
#else
    (void)window;
    (void)verbose;
    return {};
#endif
}

} // namespace hitsc
