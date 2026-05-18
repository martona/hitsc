#include "pikvm_view.hpp"

#include "diagnostics.hpp"
#include "log.hpp"
#include "pikvm_events.hpp"
#include "pikvm_session.hpp"
#include "pikvm_video.hpp"

#include <SDL3/SDL.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace hitsc {
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using Microsoft::WRL::ComPtr;

namespace {

struct PikvmViewState {
    std::mutex frame_mutex;
    std::mutex control_mutex;
    std::shared_ptr<const PikvmVideoFrame> frame;
    std::uint64_t frame_sequence = 0;
    std::string status = "starting";
    std::exception_ptr exception;
    std::function<void()> force_close;
};

struct PikvmRendererSetup {
    SDL_Renderer* renderer = nullptr;
    std::shared_ptr<PikvmD3D11Context> d3d11_context;
};

void throw_sdl_error(std::string_view context)
{
    throw std::runtime_error(std::string(context) + ": " + SDL_GetError());
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

std::shared_ptr<PikvmD3D11Context> query_renderer_d3d11_context(SDL_Renderer* renderer)
{
    SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
    if (props == 0) {
        return {};
    }

    auto* device = static_cast<ID3D11Device*>(
        SDL_GetPointerProperty(props, SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr));
    if (device == nullptr) {
        return {};
    }

    auto context = std::make_shared<PikvmD3D11Context>();
    context->device = device;
    context->lock = std::make_shared<std::recursive_mutex>();
    return context;
}

ComPtr<ID3D11DeviceContext> get_immediate_d3d11_context(
    const std::shared_ptr<PikvmD3D11Context>& d3d11_context)
{
    ComPtr<ID3D11DeviceContext> immediate_context;
    if (d3d11_context && d3d11_context->device != nullptr) {
        d3d11_context->device->GetImmediateContext(&immediate_context);
    }
    return immediate_context;
}

PikvmRendererSetup create_pikvm_renderer(SDL_Window* window, const PikvmViewOptions& options)
{
    const bool allow_d3d11 = options.video_decode != PikvmVideoDecodeMode::software;
    if (allow_d3d11) {
        std::string d3d11_error;
        SDL_Renderer* renderer = try_create_named_renderer(window, "direct3d11", d3d11_error);
        if (renderer != nullptr) {
            std::shared_ptr<PikvmD3D11Context> d3d11_context =
                query_renderer_d3d11_context(renderer);
            std::string adapter_description;
            const bool software_adapter =
                d3d11_context
                && d3d11_device_is_software_adapter(d3d11_context->device, adapter_description);

            if (d3d11_context && !software_adapter) {
                if (options.login.verbose) {
                    log_info() << "SDL renderer selected for PiKVM"
                               << " name=" << SDL_GetRendererName(renderer)
                               << " d3d11=yes"
                               << " adapter=\"" << adapter_description << "\"";
                }
                return {renderer, d3d11_context};
            }

            if (options.video_decode == PikvmVideoDecodeMode::d3d11) {
                SDL_DestroyRenderer(renderer);
                if (software_adapter) {
                    throw std::runtime_error(
                        "SDL direct3d11 renderer is using a software adapter"
                        + (adapter_description.empty() ? std::string{} : ": " + adapter_description));
                }
                throw std::runtime_error("SDL direct3d11 renderer did not expose a D3D11 device");
            }

            if (options.login.verbose) {
                if (software_adapter) {
                    log_warning() << "D3D11 hardware decode disabled for software adapter"
                                  << (adapter_description.empty() ? "" : ": ")
                                  << adapter_description;
                } else {
                    log_warning() << "D3D11 hardware decode disabled; renderer did not expose a D3D11 device";
                }
            }
            return {renderer, {}};
        }

        if (options.video_decode == PikvmVideoDecodeMode::d3d11) {
            throw std::runtime_error("failed to create SDL direct3d11 renderer: " + d3d11_error);
        }
        if (options.login.verbose) {
            log_warning() << "failed to create SDL direct3d11 renderer; using default renderer: "
                          << d3d11_error;
        }
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        throw_sdl_error("SDL_CreateRenderer");
    }
    if (options.login.verbose) {
        log_info() << "SDL renderer selected for PiKVM"
                   << " name=" << SDL_GetRendererName(renderer)
                   << " d3d11=no";
    }
    return {renderer, {}};
}

SDL_FRect centered_target_rect(int window_width, int window_height, int frame_width, int frame_height)
{
    const float width_scale = static_cast<float>(window_width) / static_cast<float>(frame_width);
    const float height_scale = static_cast<float>(window_height) / static_cast<float>(frame_height);
    const float scale = std::min(width_scale, height_scale);
    SDL_FRect rect{};
    rect.w = std::floor(static_cast<float>(frame_width) * scale);
    rect.h = std::floor(static_cast<float>(frame_height) * scale);
    rect.x = std::floor((static_cast<float>(window_width) - rect.w) / 2.0f);
    rect.y = std::floor((static_cast<float>(window_height) - rect.h) / 2.0f);
    return rect;
}

SDL_FRect current_target_rect(SDL_Window* window, int frame_width, int frame_height)
{
    int window_width = 0;
    int window_height = 0;
    if (!SDL_GetWindowSizeInPixels(window, &window_width, &window_height)) {
        SDL_GetWindowSize(window, &window_width, &window_height);
    }
    return centered_target_rect(window_width, window_height, frame_width, frame_height);
}

SDL_PixelFormat sdl_pixel_format_for_frame(PikvmVideoPixelFormat format)
{
    switch (format) {
    case PikvmVideoPixelFormat::rgba32:
        return SDL_PIXELFORMAT_RGBA32;
    case PikvmVideoPixelFormat::i420:
        return SDL_PIXELFORMAT_IYUV;
    case PikvmVideoPixelFormat::nv12:
        return SDL_PIXELFORMAT_NV12;
    case PikvmVideoPixelFormat::d3d11_nv12:
        return SDL_PIXELFORMAT_NV12;
    }
    return SDL_PIXELFORMAT_RGBA32;
}

void update_pikvm_texture(SDL_Texture* texture, const PikvmVideoFrame& frame)
{
    switch (frame.format) {
    case PikvmVideoPixelFormat::rgba32:
        if (!SDL_UpdateTexture(texture, nullptr, frame.rgba.data(), frame.width * 4)) {
            throw_sdl_error("SDL_UpdateTexture");
        }
        return;
    case PikvmVideoPixelFormat::i420:
        if (!SDL_UpdateYUVTexture(
                texture,
                nullptr,
                frame.planes[0],
                frame.pitches[0],
                frame.planes[1],
                frame.pitches[1],
                frame.planes[2],
                frame.pitches[2])) {
            throw_sdl_error("SDL_UpdateYUVTexture");
        }
        return;
    case PikvmVideoPixelFormat::nv12:
        if (!SDL_UpdateNVTexture(
                texture,
                nullptr,
                frame.planes[0],
                frame.pitches[0],
                frame.planes[1],
                frame.pitches[1])) {
            throw_sdl_error("SDL_UpdateNVTexture");
        }
        return;
    case PikvmVideoPixelFormat::d3d11_nv12:
        throw std::runtime_error("D3D11 PiKVM frames must be uploaded with the D3D11 path");
    }
}

bool d3d11_frame_can_wrap_direct(const PikvmVideoFrame& frame)
{
    if (frame.format != PikvmVideoPixelFormat::d3d11_nv12 || frame.d3d11_texture == nullptr) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    frame.d3d11_texture->GetDesc(&desc);
    return desc.ArraySize == 1
        && frame.d3d11_array_slice == 0
        && desc.Format == DXGI_FORMAT_NV12
        && (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
}

SDL_Texture* try_create_wrapped_d3d11_texture(
    SDL_Renderer* renderer,
    const PikvmVideoFrame& frame,
    std::string& error)
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
            frame.d3d11_texture);
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

ID3D11Texture2D* sdl_texture_d3d11_resource(SDL_Texture* texture)
{
    SDL_PropertiesID props = SDL_GetTextureProperties(texture);
    if (props == 0) {
        return nullptr;
    }
    return static_cast<ID3D11Texture2D*>(
        SDL_GetPointerProperty(props, SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER, nullptr));
}

void copy_d3d11_frame_to_texture(
    SDL_Texture* texture,
    const PikvmVideoFrame& frame,
    const std::shared_ptr<PikvmD3D11Context>& d3d11_context)
{
    if (!d3d11_context || d3d11_context->device == nullptr || !d3d11_context->lock) {
        throw std::runtime_error("D3D11 PiKVM frame received without a D3D11 renderer context");
    }
    if (frame.d3d11_texture == nullptr) {
        throw std::runtime_error("D3D11 PiKVM frame is missing its source texture");
    }

    ID3D11Texture2D* destination = sdl_texture_d3d11_resource(texture);
    if (destination == nullptr) {
        throw std::runtime_error("SDL texture did not expose a D3D11 texture");
    }

    D3D11_TEXTURE2D_DESC source_desc{};
    D3D11_TEXTURE2D_DESC destination_desc{};
    frame.d3d11_texture->GetDesc(&source_desc);
    destination->GetDesc(&destination_desc);
    if (source_desc.Format != DXGI_FORMAT_NV12 || destination_desc.Format != DXGI_FORMAT_NV12) {
        throw std::runtime_error("D3D11 PiKVM texture copy requires NV12 textures");
    }
    if (frame.d3d11_array_slice < 0
        || static_cast<UINT>(frame.d3d11_array_slice) >= source_desc.ArraySize) {
        throw std::runtime_error("D3D11 PiKVM frame has an invalid texture array slice");
    }

    const UINT source_subresource = D3D11CalcSubresource(
        0,
        static_cast<UINT>(frame.d3d11_array_slice),
        source_desc.MipLevels);

    std::lock_guard lock(*d3d11_context->lock);
    ComPtr<ID3D11DeviceContext> immediate_context = get_immediate_d3d11_context(d3d11_context);
    if (!immediate_context) {
        throw std::runtime_error("failed to get D3D11 immediate context");
    }
    immediate_context->CopySubresourceRegion(
        destination,
        0,
        0,
        0,
        0,
        frame.d3d11_texture,
        source_subresource,
        nullptr);
}

void set_pikvm_status(PikvmViewState& state, std::string status)
{
    std::lock_guard lock(state.control_mutex);
    state.status = std::move(status);
}

std::string pikvm_status_snapshot(PikvmViewState& state)
{
    std::lock_guard lock(state.control_mutex);
    return state.status;
}

void set_pikvm_exception(PikvmViewState& state, std::exception_ptr exception)
{
    std::lock_guard lock(state.control_mutex);
    if (!state.exception) {
        state.exception = exception;
    }
}

std::exception_ptr take_pikvm_exception(PikvmViewState& state)
{
    std::lock_guard lock(state.control_mutex);
    return state.exception;
}

void set_pikvm_force_close(PikvmViewState& state, std::function<void()> force_close)
{
    std::lock_guard lock(state.control_mutex);
    state.force_close = std::move(force_close);
}

void store_pikvm_frame(PikvmViewState& state, PikvmVideoFrame frame)
{
    std::lock_guard lock(state.frame_mutex);
    frame.sequence = ++state.frame_sequence;
    state.frame = std::make_shared<PikvmVideoFrame>(std::move(frame));
}

std::shared_ptr<const PikvmVideoFrame> take_latest_pikvm_frame(
    PikvmViewState& state,
    std::uint64_t last_sequence)
{
    std::lock_guard lock(state.frame_mutex);
    if (!state.frame || state.frame->sequence == last_sequence) {
        return {};
    }
    return state.frame;
}

void clear_pikvm_frame(PikvmViewState& state)
{
    std::shared_ptr<const PikvmVideoFrame> frame;
    {
        std::lock_guard lock(state.frame_mutex);
        frame = std::move(state.frame);
    }

    if (frame && frame->d3d11_lock) {
        std::lock_guard lock(*frame->d3d11_lock);
        frame.reset();
    }
}

void destroy_pikvm_texture(
    SDL_Texture*& texture,
    const std::shared_ptr<PikvmD3D11Context>& d3d11_context)
{
    if (texture == nullptr) {
        return;
    }

    std::unique_lock<std::recursive_mutex> d3d11_lock;
    if (d3d11_context && d3d11_context->lock) {
        d3d11_lock = std::unique_lock<std::recursive_mutex>(*d3d11_context->lock);
    }
    SDL_DestroyTexture(texture);
    texture = nullptr;
}

void destroy_pikvm_renderer(
    SDL_Renderer*& renderer,
    const std::shared_ptr<PikvmD3D11Context>& d3d11_context)
{
    if (renderer == nullptr) {
        return;
    }

    std::unique_lock<std::recursive_mutex> d3d11_lock;
    if (d3d11_context && d3d11_context->lock) {
        d3d11_lock = std::unique_lock<std::recursive_mutex>(*d3d11_context->lock);
    }
    SDL_DestroyRenderer(renderer);
    renderer = nullptr;
}

void force_close_weak_socket(std::weak_ptr<PikvmWebSocket> weak_ws)
{
    if (std::shared_ptr<PikvmWebSocket> ws = weak_ws.lock()) {
        force_close_pikvm_websocket(*ws);
    }
}

void run_pikvm_network_session(
    const PikvmViewOptions& options,
    std::shared_ptr<PikvmD3D11Context> d3d11_context,
    PikvmViewState& state,
    std::atomic_bool& stop_requested)
{
    set_pikvm_status(state, "logging in");
    PikvmSession session = login_pikvm(options.login);
    PikvmLogoutGuard logout_guard(options.login);
    logout_guard.arm(session);
    log_info() << "pikvm login succeeded";
    if (options.login.verbose) {
        log_info() << "cookies stored: " << session.cookies.size();
    }

    if (stop_requested.load()) {
        return;
    }

    asio::io_context io;
    ssl::context tls_context(ssl::context::tls_client);

    std::weak_ptr<PikvmWebSocket> weak_event_ws;
    std::weak_ptr<PikvmWebSocket> weak_video_ws;
    auto request_stop = [&] {
        stop_requested.store(true);
        force_close_weak_socket(weak_event_ws);
        force_close_weak_socket(weak_video_ws);
        io.stop();
    };

    set_pikvm_status(state, "connecting event websocket");
    std::shared_ptr<PikvmWebSocket> event_ws =
        connect_pikvm_websocket(
            io,
            tls_context,
            options.login,
            session.cookies,
            options.idle_timeout_seconds,
            "/api/ws?stream=1");
    weak_event_ws = event_ws;
    if (stop_requested.load()) {
        request_stop();
        return;
    }

    set_pikvm_status(state, "connecting video websocket");
    std::shared_ptr<PikvmWebSocket> video_ws =
        connect_pikvm_websocket(
            io,
            tls_context,
            options.login,
            session.cookies,
            options.idle_timeout_seconds,
            "/api/media/ws");
    weak_video_ws = video_ws;
    set_pikvm_force_close(state, request_stop);

    auto on_error = [&](std::exception_ptr exception) {
        set_pikvm_exception(state, exception);
        request_stop();
    };

    set_pikvm_status(state, "connected");
    start_pikvm_event_drain(event_ws, options, stop_requested, on_error);
    start_pikvm_video_stream(
        video_ws,
        options,
        std::move(d3d11_context),
        stop_requested,
        [&](PikvmVideoFrame frame) {
            store_pikvm_frame(state, std::move(frame));
        },
        on_error);

    io.run();
    set_pikvm_force_close(state, {});
    if (stop_requested.load()) {
        set_pikvm_status(state, "stopped");
    }
}

void stop_pikvm_network(
    PikvmViewState& state,
    std::atomic_bool& stop_requested,
    std::thread& network_thread,
    bool verbose)
{
    if (verbose) {
        log_debug() << "pikvm network stop begin";
    }

    stop_requested.store(true);
    std::function<void()> force_close;
    {
        std::lock_guard lock(state.control_mutex);
        force_close = state.force_close;
    }
    if (force_close) {
        force_close();
    }

    if (network_thread.joinable()) {
        network_thread.join();
    }

    if (verbose) {
        log_debug() << "pikvm network stop end";
    }
}

} // namespace

void run_pikvm_view(const PikvmViewOptions& options)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw_sdl_error("SDL_Init");
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    std::shared_ptr<PikvmD3D11Context> d3d11_context;
    auto state = std::make_shared<PikvmViewState>();
    auto stop_requested = std::make_shared<std::atomic_bool>(false);
    auto network_done = std::make_shared<std::atomic_bool>(false);
    std::thread network_thread;

    try {
        window = SDL_CreateWindow("hitsc - PiKVM", 1024, 768, SDL_WINDOW_RESIZABLE);
        if (window == nullptr) {
            throw_sdl_error("SDL_CreateWindow");
        }

        PikvmRendererSetup renderer_setup = create_pikvm_renderer(window, options);
        renderer = renderer_setup.renderer;
        d3d11_context = renderer_setup.d3d11_context;

        PikvmViewOptions network_options = options;
        if (!d3d11_context && network_options.video_decode == PikvmVideoDecodeMode::auto_select) {
            network_options.video_decode = PikvmVideoDecodeMode::software;
        }
        network_thread = std::thread([network_options, d3d11_context, state, stop_requested, network_done] {
            try {
                run_pikvm_network_session(network_options, d3d11_context, *state, *stop_requested);
            } catch (...) {
                set_pikvm_exception(*state, std::current_exception());
            }
            set_pikvm_force_close(*state, {});
            network_done->store(true);
        });

        bool running = true;
        bool first_render = true;
        bool close_event_logged = false;
        std::uint64_t last_sequence = 0;
        std::uint64_t last_status_tick = 0;
        int texture_width = 0;
        int texture_height = 0;
        int title_width = 0;
        int title_height = 0;
        PikvmVideoPixelFormat texture_format = PikvmVideoPixelFormat::rgba32;
        bool texture_wraps_d3d11_source = false;
        bool d3d11_direct_wrap_disabled = false;
        ID3D11Texture2D* texture_wrapped_d3d11_source = nullptr;
        int presented_frames = 0;

        while (running) {
            bool render_needed = first_render;
            SDL_Event event{};
            bool have_event = SDL_WaitEventTimeout(&event, 16);
            while (have_event) {
                if (event.type == SDL_EVENT_QUIT ||
                    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    if (!close_event_logged) {
                        close_event_logged = true;
                        log_info() << "pikvm window close event"
                                   << " type=" << event.type;
                    }
                    running = false;
                } else if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                           event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                           event.type == SDL_EVENT_WINDOW_EXPOSED) {
                    render_needed = true;
                }
                have_event = SDL_PollEvent(&event);
            }

            const std::shared_ptr<const PikvmVideoFrame> frame =
                take_latest_pikvm_frame(*state, last_sequence);
            if (frame) {
                std::unique_lock<std::recursive_mutex> d3d11_render_lock;
                if (d3d11_context && d3d11_context->lock) {
                    d3d11_render_lock = std::unique_lock<std::recursive_mutex>(*d3d11_context->lock);
                }

                last_sequence = frame->sequence;
                if (frame->format == PikvmVideoPixelFormat::d3d11_nv12) {
                    const bool direct_wrap =
                        !d3d11_direct_wrap_disabled && d3d11_frame_can_wrap_direct(*frame);
                    if (direct_wrap) {
                        if (texture == nullptr
                            || texture_width != frame->width
                            || texture_height != frame->height
                            || texture_format != frame->format
                            || !texture_wraps_d3d11_source
                            || texture_wrapped_d3d11_source != frame->d3d11_texture) {
                            if (texture != nullptr) {
                                SDL_DestroyTexture(texture);
                            }

                            std::string wrap_error;
                            texture = try_create_wrapped_d3d11_texture(renderer, *frame, wrap_error);
                            if (texture != nullptr) {
                                texture_width = frame->width;
                                texture_height = frame->height;
                                texture_format = frame->format;
                                texture_wraps_d3d11_source = true;
                                texture_wrapped_d3d11_source = frame->d3d11_texture;
                                if (options.login.verbose) {
                                    log_info() << "wrapped PiKVM D3D11 video texture directly";
                                }
                            } else {
                                d3d11_direct_wrap_disabled = true;
                                texture_width = 0;
                                texture_height = 0;
                                texture_format = PikvmVideoPixelFormat::rgba32;
                                texture_wraps_d3d11_source = false;
                                texture_wrapped_d3d11_source = nullptr;
                                if (options.login.verbose) {
                                    log_warning() << "direct D3D11 texture wrap failed; using GPU copy: "
                                                  << wrap_error;
                                }
                            }
                        }
                    }

                    if (texture == nullptr || !texture_wraps_d3d11_source) {
                        if (texture == nullptr
                            || texture_width != frame->width
                            || texture_height != frame->height
                            || texture_format != frame->format
                            || texture_wraps_d3d11_source) {
                            if (texture != nullptr) {
                                SDL_DestroyTexture(texture);
                            }
                            texture = SDL_CreateTexture(
                                renderer,
                                SDL_PIXELFORMAT_NV12,
                                SDL_TEXTUREACCESS_STATIC,
                                frame->width,
                                frame->height);
                            if (texture == nullptr) {
                                throw_sdl_error("SDL_CreateTexture(D3D11 NV12)");
                            }
                            if (sdl_texture_d3d11_resource(texture) == nullptr) {
                                throw std::runtime_error("SDL NV12 texture did not expose a D3D11 resource");
                            }
                            texture_width = frame->width;
                            texture_height = frame->height;
                            texture_format = frame->format;
                            texture_wraps_d3d11_source = false;
                            texture_wrapped_d3d11_source = nullptr;
                        }

                        copy_d3d11_frame_to_texture(texture, *frame, d3d11_context);
                    }
                } else {
                    if (texture == nullptr
                        || texture_width != frame->width
                        || texture_height != frame->height
                        || texture_format != frame->format
                        || texture_wraps_d3d11_source) {
                        if (texture != nullptr) {
                            SDL_DestroyTexture(texture);
                        }
                        texture = SDL_CreateTexture(
                            renderer,
                            sdl_pixel_format_for_frame(frame->format),
                            SDL_TEXTUREACCESS_STREAMING,
                            frame->width,
                            frame->height);
                        if (texture == nullptr) {
                            throw_sdl_error("SDL_CreateTexture");
                        }
                        texture_width = frame->width;
                        texture_height = frame->height;
                        texture_format = frame->format;
                        texture_wraps_d3d11_source = false;
                        texture_wrapped_d3d11_source = nullptr;
                    }

                    update_pikvm_texture(texture, *frame);
                }

                if (title_width != frame->width || title_height != frame->height) {
                    title_width = frame->width;
                    title_height = frame->height;
                    SDL_SetWindowTitle(
                        window,
                        ("hitsc - PiKVM - " + options.login.base_url.host + " - "
                         + std::to_string(frame->width) + "x" + std::to_string(frame->height))
                            .c_str());
                }

                ++presented_frames;
                if (options.login.verbose && (presented_frames <= 20 || presented_frames % 60 == 0)) {
                    log_info() << "presented PiKVM frame #" << presented_frames
                               << " sequence=" << frame->sequence
                               << " format=" << pikvm_video_pixel_format_name(frame->format);
                }
                render_needed = true;
            }

            if (render_needed) {
                std::unique_lock<std::recursive_mutex> d3d11_render_lock;
                if (d3d11_context && d3d11_context->lock) {
                    d3d11_render_lock = std::unique_lock<std::recursive_mutex>(*d3d11_context->lock);
                }

                SDL_SetRenderDrawColor(renderer, 12, 14, 18, 255);
                SDL_RenderClear(renderer);
                if (texture != nullptr && texture_width > 0 && texture_height > 0) {
                    const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                    SDL_RenderTexture(renderer, texture, nullptr, &target);
                }
                SDL_RenderPresent(renderer);
                first_render = false;
            }

            const std::uint64_t ticks = SDL_GetTicks();
            if (texture == nullptr && ticks - last_status_tick > 1000) {
                last_status_tick = ticks;
                SDL_SetWindowTitle(window, ("hitsc - PiKVM - " + pikvm_status_snapshot(*state)).c_str());
            }

            if (network_done->load()) {
                if (std::exception_ptr exception = take_pikvm_exception(*state)) {
                    std::rethrow_exception(exception);
                }
                running = false;
            }
        }
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "pikvm view ui thread");
        stop_pikvm_network(*state, *stop_requested, network_thread, options.login.verbose);
        clear_pikvm_frame(*state);
        destroy_pikvm_texture(texture, d3d11_context);
        destroy_pikvm_renderer(renderer, d3d11_context);
        if (window != nullptr) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        d3d11_context.reset();
        SDL_Quit();
        throw;
    }

    stop_pikvm_network(*state, *stop_requested, network_thread, options.login.verbose);

    clear_pikvm_frame(*state);
    destroy_pikvm_texture(texture, d3d11_context);
    destroy_pikvm_renderer(renderer, d3d11_context);
    SDL_DestroyWindow(window);
    window = nullptr;
    d3d11_context.reset();
    SDL_Quit();
}

} // namespace hitsc
