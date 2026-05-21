#include "pikvm_view.hpp"

#include "diagnostics.hpp"
#include "errors.hpp"
#include "log.hpp"
#include "pikvm_events.hpp"
#include "pikvm_input.hpp"
#include "pikvm_session.hpp"
#include "pikvm_video.hpp"
#include "view_base.hpp"

#include <SDL3/SDL.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cwchar>
#include <cstdint>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace hitsc {
using Microsoft::WRL::ComPtr;

namespace {

using PikvmClock = std::chrono::steady_clock;

constexpr std::uint64_t kPikvmMouseMotionIntervalMilliseconds = 8;
constexpr std::chrono::milliseconds kPikvmInputStopGrace{250};
constexpr std::size_t kPikvmMouseButtonSlots = 8;

struct PikvmViewState : ViewStateBase {
    LatestMailbox<PikvmVideoFrame> frames;
    InputQueue<PikvmInputWork> input;
    std::atomic_bool video_decode_paused{false};
    std::string status = "starting";
};

struct DurationStats {
    std::uint64_t count = 0;
    std::chrono::microseconds total{};
    std::chrono::microseconds max{};

    void add(PikvmClock::duration duration)
    {
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration);
        if (micros.count() < 0) {
            return;
        }
        ++count;
        total += micros;
        max = std::max(max, micros);
    }

    double average_ms() const
    {
        if (count == 0) {
            return 0.0;
        }
        return static_cast<double>(total.count()) / static_cast<double>(count) / 1000.0;
    }

    double max_ms() const
    {
        return static_cast<double>(max.count()) / 1000.0;
    }
};

struct PikvmFrameLatencyBatch {
    std::uint64_t frames = 0;
    std::uint64_t payload_bytes = 0;
    DurationStats receive_to_decode;
    DurationStats decode_to_store;
    DurationStats store_to_present;
    DurationStats receive_to_present;
    int last_width = 0;
    int last_height = 0;
    PikvmVideoPixelFormat last_format = PikvmVideoPixelFormat::rgba32;

    void clear()
    {
        *this = {};
    }
};

struct PikvmNetworkStopHandles {
    std::mutex mutex;
    std::function<void()> control_stop;
    std::function<void()> video_stop;

    void set_control(std::function<void()> stop)
    {
        std::lock_guard lock(mutex);
        control_stop = std::move(stop);
    }

    void set_video(std::function<void()> stop)
    {
        std::lock_guard lock(mutex);
        video_stop = std::move(stop);
    }

    void stop_all()
    {
        std::function<void()> control;
        std::function<void()> video;
        {
            std::lock_guard lock(mutex);
            control = control_stop;
            video = video_stop;
        }
        if (control) {
            control();
        }
        if (video) {
            video();
        }
    }
};

struct PikvmRendererSetup {
    SDL_Renderer* renderer = nullptr;
    std::shared_ptr<PikvmD3D11Context> d3d11_context;
};

using PikvmKeyDownState = std::array<bool, SDL_SCANCODE_COUNT>;
using PikvmMouseButtonDownState = std::array<bool, kPikvmMouseButtonSlots>;

void throw_sdl_error(std::string_view context)
{
    throw std::runtime_error(std::string(context) + ": " + SDL_GetError());
}

std::string format_ms(double value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
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
                    throw UserError(
                        "SDL direct3d11 renderer is using a software adapter"
                        + (adapter_description.empty() ? std::string{} : ": " + adapter_description));
                }
                throw UserError("SDL direct3d11 renderer did not expose a D3D11 device");
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
            throw UserError("failed to create SDL direct3d11 renderer: " + d3d11_error);
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

std::optional<PikvmAbsoluteMousePosition> pikvm_mouse_position(
    float window_x,
    float window_y,
    const SDL_FRect& target,
    bool clamp_to_target)
{
    if (target.w <= 0.0f || target.h <= 0.0f) {
        return std::nullopt;
    }

    const bool inside =
        window_x >= target.x
        && window_y >= target.y
        && window_x <= target.x + target.w
        && window_y <= target.y + target.h;
    if (!inside && !clamp_to_target) {
        return std::nullopt;
    }

    const float clamped_x = std::clamp(window_x, target.x, target.x + target.w);
    const float clamped_y = std::clamp(window_y, target.y, target.y + target.h);
    const double normalized_x =
        (static_cast<double>(clamped_x) - static_cast<double>(target.x)) / static_cast<double>(target.w);
    const double normalized_y =
        (static_cast<double>(clamped_y) - static_cast<double>(target.y)) / static_cast<double>(target.h);
    return make_pikvm_absolute_mouse_position(normalized_x, normalized_y);
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

void queue_pikvm_key_event(
    PikvmViewState& state,
    std::string_view code,
    bool pressed)
{
    state.input.enqueue(PikvmInputWork{make_pikvm_key_packet(code, pressed)});
}

void queue_pikvm_mouse_button_event(
    PikvmViewState& state,
    std::string_view button,
    bool pressed)
{
    state.input.enqueue(PikvmInputWork{make_pikvm_mouse_button_packet(button, pressed)});
}

void queue_pikvm_mouse_move_event(
    PikvmViewState& state,
    const PikvmAbsoluteMousePosition& position)
{
    state.input.enqueue(PikvmInputWork{make_pikvm_mouse_move_packet(position)});
}

void queue_pikvm_mouse_wheel_event(
    PikvmViewState& state,
    int delta_x,
    int delta_y)
{
    state.input.enqueue(PikvmInputWork{make_pikvm_mouse_wheel_packet(delta_x, delta_y)});
}

bool any_pikvm_mouse_button_down(const PikvmMouseButtonDownState& mouse_down)
{
    return std::any_of(mouse_down.begin(), mouse_down.end(), [](bool down) {
        return down;
    });
}

bool pikvm_mouse_button_slot(std::uint8_t button, std::size_t& slot)
{
    slot = static_cast<std::size_t>(button);
    return slot < kPikvmMouseButtonSlots;
}

void release_all_pikvm_keys(
    PikvmViewState& state,
    PikvmKeyDownState& key_down)
{
    for (std::size_t scancode = 0; scancode < key_down.size(); ++scancode) {
        if (!key_down[scancode]) {
            continue;
        }
        key_down[scancode] = false;
        const auto code = pikvm_key_code_from_sdl_scancode(static_cast<SDL_Scancode>(scancode));
        if (code) {
            queue_pikvm_key_event(state, *code, false);
        }
    }
}

void clear_pikvm_local_mouse_capture(PikvmMouseButtonDownState& mouse_down)
{
    mouse_down.fill(false);
    SDL_CaptureMouse(false);
}

void store_pikvm_frame(PikvmViewState& state, PikvmVideoFrame frame)
{
    frame.timing.stored_at = PikvmClock::now();
    state.frames.publish(std::move(frame));
    state.push_render_event();
}

void release_pikvm_latest_frame(PikvmViewState& state)
{
    std::shared_ptr<const PikvmVideoFrame> frame = state.frames.clear();

    if (frame && frame->d3d11_lock) {
        std::lock_guard lock(*frame->d3d11_lock);
        frame.reset();
    }
}

void add_pikvm_frame_latency(
    PikvmFrameLatencyBatch& batch,
    const PikvmVideoFrame& frame,
    PikvmClock::time_point presented_at)
{
    ++batch.frames;
    batch.payload_bytes += pikvm_video_frame_payload_bytes(frame);
    batch.last_width = frame.width;
    batch.last_height = frame.height;
    batch.last_format = frame.format;

    const PikvmVideoFrameTiming& timing = frame.timing;
    if (timing.media_received_at != PikvmClock::time_point{} &&
        timing.decoded_at != PikvmClock::time_point{}) {
        batch.receive_to_decode.add(timing.decoded_at - timing.media_received_at);
        batch.receive_to_present.add(presented_at - timing.media_received_at);
    }
    if (timing.decoded_at != PikvmClock::time_point{} &&
        timing.stored_at != PikvmClock::time_point{}) {
        batch.decode_to_store.add(timing.stored_at - timing.decoded_at);
    }
    if (timing.stored_at != PikvmClock::time_point{}) {
        batch.store_to_present.add(presented_at - timing.stored_at);
    }
}

void maybe_log_pikvm_frame_latency(
    PikvmFrameLatencyBatch& batch,
    PikvmClock::time_point& last_log,
    bool force)
{
    const auto now = PikvmClock::now();
    if (batch.frames == 0) {
        return;
    }
    if (!force && batch.frames < 60 && now - last_log < std::chrono::seconds(2)) {
        return;
    }

    log_info() << "pikvm frame latency"
               << " frames=" << batch.frames
               << " payload-bytes=" << batch.payload_bytes
               << " size=" << batch.last_width << 'x' << batch.last_height
               << " format=" << pikvm_video_pixel_format_name(batch.last_format)
               << " receive-decode-avg-ms=" << format_ms(batch.receive_to_decode.average_ms())
               << " receive-decode-max-ms=" << format_ms(batch.receive_to_decode.max_ms())
               << " decode-store-avg-ms=" << format_ms(batch.decode_to_store.average_ms())
               << " decode-store-max-ms=" << format_ms(batch.decode_to_store.max_ms())
               << " store-present-avg-ms=" << format_ms(batch.store_to_present.average_ms())
               << " store-present-max-ms=" << format_ms(batch.store_to_present.max_ms())
               << " receive-present-avg-ms=" << format_ms(batch.receive_to_present.average_ms())
               << " receive-present-max-ms=" << format_ms(batch.receive_to_present.max_ms());
    batch.clear();
    last_log = now;
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

void reset_pikvm_texture_state(
    SDL_Texture*& texture,
    int& texture_width,
    int& texture_height,
    PikvmVideoPixelFormat& texture_format,
    bool& texture_wraps_d3d11_source,
    ID3D11Texture2D*& texture_wrapped_d3d11_source,
    const std::shared_ptr<PikvmD3D11Context>& d3d11_context)
{
    destroy_pikvm_texture(texture, d3d11_context);
    texture_width = 0;
    texture_height = 0;
    texture_format = PikvmVideoPixelFormat::rgba32;
    texture_wraps_d3d11_source = false;
    texture_wrapped_d3d11_source = nullptr;
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

struct PikvmControlStopState {
    std::weak_ptr<PikvmEventSession> session;
    std::atomic_bool* stop_requested = nullptr;
    BmcWebSession* web = nullptr;
};

struct PikvmVideoStopState {
    std::atomic_bool* stop_requested = nullptr;
    BmcWebSession* web = nullptr;
};

void request_pikvm_control_stop(const std::shared_ptr<PikvmControlStopState>& stop_state)
{
    if (!stop_state) {
        return;
    }
    if (stop_state->stop_requested != nullptr) {
        stop_state->stop_requested->store(true);
    }
    if (std::shared_ptr<PikvmEventSession> event_session = stop_state->session.lock()) {
        stop_pikvm_event_session(event_session, kPikvmInputStopGrace);
    } else if (stop_state->web != nullptr) {
        stop_state->web->force_close_websocket("pikvm-control");
    }
}

void request_pikvm_video_stop(const std::shared_ptr<PikvmVideoStopState>& stop_state)
{
    if (!stop_state) {
        return;
    }
    if (stop_state->stop_requested != nullptr) {
        stop_state->stop_requested->store(true);
    }
    if (stop_state->web != nullptr) {
        stop_state->web->force_close_websocket("pikvm-video");
    }
}

void run_pikvm_control_worker(
    PikvmViewOptions options,
    BmcWebSocketConnectionPtr websocket,
    PikvmViewState& state,
    std::atomic_bool& stop_requested,
    std::shared_ptr<PikvmControlStopState> stop_state,
    std::function<void(std::exception_ptr)> on_error)
{
    try {
        std::shared_ptr<PikvmWebSocket> event_ws = websocket->stream();

        if (stop_requested.load()) {
            if (stop_state->web != nullptr) {
                stop_state->web->force_close_websocket("pikvm-control");
            }
            return;
        }

        state.view_status.kvm_connection(true);
        std::shared_ptr<PikvmEventSession> event_session =
            start_pikvm_event_session(
                event_ws,
                options,
                stop_requested,
                [&](bool online) {
                    state.view_status.kvm_display_status(online);
                },
                on_error);
        stop_state->session = event_session;
        state.input.install(make_pikvm_event_input_sink(event_session));
        set_pikvm_status(state, "control websocket connected");
        websocket->io_context().run();
    } catch (...) {
        if (!stop_requested.load()) {
            on_error(std::current_exception());
        }
    }

    state.view_status.kvm_connection(false);
}

void run_pikvm_video_worker(
    PikvmViewOptions options,
    BmcWebSocketConnectionPtr websocket,
    std::shared_ptr<PikvmD3D11Context> d3d11_context,
    PikvmViewState& state,
    std::atomic_bool& stop_requested,
    std::shared_ptr<PikvmVideoStopState> stop_state,
    std::function<void(std::exception_ptr)> on_error)
{
    try {
        std::shared_ptr<PikvmWebSocket> video_ws = websocket->stream();

        if (stop_requested.load()) {
            if (stop_state->web != nullptr) {
                stop_state->web->force_close_websocket("pikvm-video");
            }
            return;
        }

        start_pikvm_video_stream(
            video_ws,
            options,
            std::move(d3d11_context),
            stop_requested,
            state.video_decode_paused,
            [&](std::size_t bytes) {
                state.view_status.data_received(bytes);
            },
            [&](PikvmVideoFrame frame) {
                if (!state.video_decode_paused.load()) {
                    store_pikvm_frame(state, std::move(frame));
                }
            },
            on_error);
        set_pikvm_status(state, "connected");
        websocket->io_context().run();
    } catch (...) {
        if (!stop_requested.load()) {
            on_error(std::current_exception());
        }
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
        log_info() << "cookies stored: " << session.web.cookie_count();
    }

    if (stop_requested.load()) {
        return;
    }

    auto stop_handles = std::make_shared<PikvmNetworkStopHandles>();
    auto request_stop = [stop_handles, &stop_requested] {
        stop_requested.store(true);
        stop_handles->stop_all();
    };
    state.set_force_close(request_stop);

    auto on_error = [&](std::exception_ptr exception) {
        state.set_exception(exception);
        request_stop();
    };

    auto control_stop_state = std::make_shared<PikvmControlStopState>();
    control_stop_state->stop_requested = &stop_requested;
    control_stop_state->web = &session.web;
    auto video_stop_state = std::make_shared<PikvmVideoStopState>();
    video_stop_state->stop_requested = &stop_requested;
    video_stop_state->web = &session.web;

    std::thread control_thread;
    std::thread video_thread;
    try {
        if (stop_requested.load()) {
            return;
        }

        set_pikvm_status(state, "connecting control websocket");
        BmcWebSocketOpenResult control_websocket = session.web.open_websocket(BmcWebSocketConnectOptions{
            .role = "pikvm-control",
            .log_name = "pikvm websocket",
            .path = "/api/ws?stream=1",
            .idle_timeout_seconds = options.idle_timeout_seconds,
            .tcp_no_delay = true,
        });
        stop_handles->set_control([control_stop_state] {
            request_pikvm_control_stop(control_stop_state);
        });

        if (stop_requested.load()) {
            return;
        }

        set_pikvm_status(state, "connecting video websocket");
        BmcWebSocketOpenResult video_websocket = session.web.open_websocket(BmcWebSocketConnectOptions{
            .role = "pikvm-video",
            .log_name = "pikvm websocket",
            .path = "/api/media/ws",
            .idle_timeout_seconds = options.idle_timeout_seconds,
            .tcp_no_delay = true,
        });

        stop_handles->set_video([video_stop_state] {
            request_pikvm_video_stop(video_stop_state);
        });

        control_thread = std::thread(
            run_pikvm_control_worker,
            options,
            control_websocket.connection,
            std::ref(state),
            std::ref(stop_requested),
            control_stop_state,
            on_error);
        video_thread = std::thread(
            run_pikvm_video_worker,
            options,
            video_websocket.connection,
            std::move(d3d11_context),
            std::ref(state),
            std::ref(stop_requested),
            video_stop_state,
            on_error);

        control_thread.join();
        video_thread.join();
    } catch (...) {
        request_stop();
        if (control_thread.joinable()) {
            control_thread.join();
        }
        if (video_thread.joinable()) {
            video_thread.join();
        }
        state.input.clear();
        state.set_force_close({});
        throw;
    }

    stop_handles->set_control({});
    stop_handles->set_video({});
    state.input.clear();
    state.set_force_close({});
    if (stop_requested.load()) {
        set_pikvm_status(state, "stopped");
    }
}

class PikvmView : public KvmViewBase {
public:
    explicit PikvmView(const PikvmViewOptions& options)
        : PikvmView(options, std::make_shared<PikvmViewState>())
    {
    }

private:
    PikvmView(const PikvmViewOptions& options, std::shared_ptr<PikvmViewState> state)
        : KvmViewBase(*state, options.login.base_url.host, "pikvm", [state] {
              state->input.clear();
          })
        , options_(options)
        , network_options_(options)
        , state_(std::move(state))
    {
    }

    SDL_Renderer* create_renderer(SDL_Window* window) override
    {
        PikvmRendererSetup renderer_setup = create_pikvm_renderer(window, options_);
        d3d11_context_ = renderer_setup.d3d11_context;
        if (!d3d11_context_ && network_options_.video_decode == PikvmVideoDecodeMode::auto_select) {
            network_options_.video_decode = PikvmVideoDecodeMode::software;
        }
        return renderer_setup.renderer;
    }

    void destroy_renderer(SDL_Renderer* renderer) override
    {
        SDL_Renderer* renderer_to_destroy = renderer;
        destroy_pikvm_renderer(renderer_to_destroy, d3d11_context_);
        d3d11_context_.reset();
    }

    void start_network(KvmNetworkWorker& network) override
    {
        PikvmViewOptions network_options = network_options_;
        std::shared_ptr<PikvmViewState> state = state_;
        network.start([network_options, d3d11_context = d3d11_context_, state](
                          std::atomic_bool& stop_requested) {
            run_pikvm_network_session(network_options, d3d11_context, *state, stop_requested);
        });
    }

    void before_sdl_cleanup() override
    {
        clear_pikvm_local_mouse_capture(mouse_down_);
        if (options_.login.vverbose) {
            maybe_log_pikvm_frame_latency(frame_latency_, last_frame_latency_log_, true);
        }
        pending_present_latency_frame_.reset();
        release_pikvm_latest_frame(*state_);
        destroy_pikvm_texture(texture_, d3d11_context_);
    }

    void on_close() override
    {
        clear_pikvm_local_mouse_capture(mouse_down_);
    }

    void on_minimized() override
    {
        state_->video_decode_paused.store(true);
        pending_present_latency_frame_.reset();
        release_pikvm_latest_frame(*state_);
        reset_pikvm_texture_state(
            texture_,
            texture_width_,
            texture_height_,
            texture_format_,
            texture_wraps_d3d11_source_,
            texture_wrapped_d3d11_source_,
            d3d11_context_);
        last_sequence_ = 0;
    }

    void on_restored() override
    {
        state_->video_decode_paused.store(false);
    }

    void on_focus_lost() override
    {
        release_all_pikvm_keys(*state_, key_down_);
    }

    void handle_event(const SDL_Event& event, bool&) override
    {
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            handle_key_event(event);
        } else if (texture_width_ > 0 && texture_height_ > 0 &&
                   (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                    event.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
            handle_mouse_button_event(event);
        } else if (texture_width_ > 0 && texture_height_ > 0 &&
                   event.type == SDL_EVENT_MOUSE_MOTION) {
            handle_mouse_motion_event(event);
        } else if (texture_width_ > 0 && texture_height_ > 0 &&
                   event.type == SDL_EVENT_MOUSE_WHEEL) {
            handle_mouse_wheel_event(event);
        }
    }

    void render_visible(bool& render_needed, bool& first_render) override
    {
        upload_latest_frame(render_needed);
        if (!render_needed || state_->video_decode_paused.load()) {
            return;
        }

        std::unique_lock<std::recursive_mutex> d3d11_render_lock;
        if (d3d11_context_ && d3d11_context_->lock) {
            d3d11_render_lock = std::unique_lock<std::recursive_mutex>(*d3d11_context_->lock);
        }

        clear_background();
        if (texture_ != nullptr && texture_width_ > 0 && texture_height_ > 0) {
            const SDL_FRect target = current_target_rect(texture_width_, texture_height_);
            SDL_RenderTexture(renderer(), texture_, nullptr, &target);
        }
        present();

        if (pending_present_latency_frame_) {
            const auto presented_at = PikvmClock::now();
            frame_presented(
                pending_present_latency_frame_->width,
                pending_present_latency_frame_->height);
            if (options_.login.vverbose) {
                add_pikvm_frame_latency(
                    frame_latency_,
                    *pending_present_latency_frame_,
                    presented_at);
                maybe_log_pikvm_frame_latency(frame_latency_, last_frame_latency_log_, false);
            }
            pending_present_latency_frame_.reset();
        }
        first_render = false;
    }

    void handle_key_event(const SDL_Event& event)
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat) {
            return;
        }

        const std::optional<std::string_view> code =
            pikvm_key_code_from_sdl_scancode(event.key.scancode);
        const auto scancode = static_cast<std::size_t>(event.key.scancode);
        if (!code || scancode >= key_down_.size()) {
            if (options_.login.vverbose) {
                log_info() << "ignored PiKVM key"
                           << " scancode=" << event.key.scancode
                           << " key=" << event.key.key;
            }
            return;
        }

        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (key_down_[scancode] != down) {
            key_down_[scancode] = down;
            queue_pikvm_key_event(*state_, *code, down);
        }
    }

    void handle_mouse_button_event(const SDL_Event& event)
    {
        const std::optional<std::string_view> button =
            pikvm_mouse_button_from_sdl_button(event.button.button);
        std::size_t button_slot = 0;
        if (!button || !pikvm_mouse_button_slot(event.button.button, button_slot)) {
            if (options_.login.vverbose) {
                log_info() << "ignored PiKVM mouse button"
                           << " button=" << static_cast<int>(event.button.button);
            }
            return;
        }

        const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        const SDL_FRect target = current_target_rect(texture_width_, texture_height_);
        const bool drag_active = any_pikvm_mouse_button_down(mouse_down_);
        const std::optional<PikvmAbsoluteMousePosition> position = pikvm_mouse_position(
            event.button.x,
            event.button.y,
            target,
            drag_active || !down);

        if (down && !position) {
            return;
        }
        if (position) {
            queue_pikvm_mouse_move_event(*state_, *position);
        }

        if (mouse_down_[button_slot] != down) {
            mouse_down_[button_slot] = down;
            queue_pikvm_mouse_button_event(*state_, *button, down);
        }
        SDL_CaptureMouse(any_pikvm_mouse_button_down(mouse_down_));
    }

    void handle_mouse_motion_event(const SDL_Event& event)
    {
        const bool drag_active = any_pikvm_mouse_button_down(mouse_down_);
        const std::uint64_t ticks = SDL_GetTicks();
        const bool throttled =
            !drag_active
            && ticks - last_mouse_motion_ticks_ < kPikvmMouseMotionIntervalMilliseconds;
        if (throttled) {
            return;
        }

        const SDL_FRect target = current_target_rect(texture_width_, texture_height_);
        const std::optional<PikvmAbsoluteMousePosition> position = pikvm_mouse_position(
            event.motion.x,
            event.motion.y,
            target,
            drag_active);
        if (position) {
            queue_pikvm_mouse_move_event(*state_, *position);
            last_mouse_motion_ticks_ = ticks;
        }
    }

    void handle_mouse_wheel_event(const SDL_Event& event)
    {
        const SDL_FRect target = current_target_rect(texture_width_, texture_height_);
        const std::optional<PikvmAbsoluteMousePosition> position = pikvm_mouse_position(
            event.wheel.mouse_x,
            event.wheel.mouse_y,
            target,
            any_pikvm_mouse_button_down(mouse_down_));
        if (!position) {
            return;
        }

        queue_pikvm_mouse_move_event(*state_, *position);
        const float x = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
            ? -event.wheel.x
            : event.wheel.x;
        const float y = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
            ? -event.wheel.y
            : event.wheel.y;
        const int delta_x = x == 0.0f ? 0 : (x > 0.0f ? 5 : -5);
        const int delta_y = y == 0.0f ? 0 : (y > 0.0f ? 5 : -5);
        if (delta_x != 0 || delta_y != 0) {
            queue_pikvm_mouse_wheel_event(*state_, delta_x, delta_y);
        }
    }

    void upload_latest_frame(bool& render_needed)
    {
        const std::shared_ptr<const PikvmVideoFrame> frame =
            state_->video_decode_paused.load()
                ? nullptr
                : state_->frames.latest(last_sequence_);
        if (!frame) {
            return;
        }

        std::unique_lock<std::recursive_mutex> d3d11_render_lock;
        if (d3d11_context_ && d3d11_context_->lock) {
            d3d11_render_lock = std::unique_lock<std::recursive_mutex>(*d3d11_context_->lock);
        }

        last_sequence_ = frame->sequence;
        if (frame->format == PikvmVideoPixelFormat::d3d11_nv12) {
            upload_d3d11_frame(*frame);
        } else {
            upload_software_frame(*frame);
        }

        pending_present_latency_frame_ = frame;
        render_needed = true;
    }

    void upload_d3d11_frame(const PikvmVideoFrame& frame)
    {
        const bool direct_wrap =
            !d3d11_direct_wrap_disabled_ && d3d11_frame_can_wrap_direct(frame);
        if (direct_wrap) {
            try_wrap_d3d11_frame(frame);
        }

        if (texture_ == nullptr || !texture_wraps_d3d11_source_) {
            ensure_d3d11_copy_texture(frame);
            copy_d3d11_frame_to_texture(texture_, frame, d3d11_context_);
        }
    }

    void try_wrap_d3d11_frame(const PikvmVideoFrame& frame)
    {
        if (texture_ != nullptr
            && texture_width_ == frame.width
            && texture_height_ == frame.height
            && texture_format_ == frame.format
            && texture_wraps_d3d11_source_
            && texture_wrapped_d3d11_source_ == frame.d3d11_texture) {
            return;
        }

        if (texture_ != nullptr) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }

        std::string wrap_error;
        texture_ = try_create_wrapped_d3d11_texture(renderer(), frame, wrap_error);
        if (texture_ != nullptr) {
            texture_width_ = frame.width;
            texture_height_ = frame.height;
            texture_format_ = frame.format;
            texture_wraps_d3d11_source_ = true;
            texture_wrapped_d3d11_source_ = frame.d3d11_texture;
            if (options_.login.verbose) {
                log_info() << "wrapped PiKVM D3D11 video texture directly";
            }
            return;
        }

        d3d11_direct_wrap_disabled_ = true;
        texture_width_ = 0;
        texture_height_ = 0;
        texture_format_ = PikvmVideoPixelFormat::rgba32;
        texture_wraps_d3d11_source_ = false;
        texture_wrapped_d3d11_source_ = nullptr;
        if (options_.login.verbose) {
            log_warning() << "direct D3D11 texture wrap failed; using GPU copy: "
                          << wrap_error;
        }
    }

    void ensure_d3d11_copy_texture(const PikvmVideoFrame& frame)
    {
        if (texture_ != nullptr
            && texture_width_ == frame.width
            && texture_height_ == frame.height
            && texture_format_ == frame.format
            && !texture_wraps_d3d11_source_) {
            return;
        }

        if (texture_ != nullptr) {
            SDL_DestroyTexture(texture_);
        }
        texture_ = SDL_CreateTexture(
            renderer(),
            SDL_PIXELFORMAT_NV12,
            SDL_TEXTUREACCESS_STATIC,
            frame.width,
            frame.height);
        if (texture_ == nullptr) {
            throw_sdl_error("SDL_CreateTexture(D3D11 NV12)");
        }
        if (sdl_texture_d3d11_resource(texture_) == nullptr) {
            throw std::runtime_error("SDL NV12 texture did not expose a D3D11 resource");
        }
        texture_width_ = frame.width;
        texture_height_ = frame.height;
        texture_format_ = frame.format;
        texture_wraps_d3d11_source_ = false;
        texture_wrapped_d3d11_source_ = nullptr;
    }

    void upload_software_frame(const PikvmVideoFrame& frame)
    {
        if (texture_ == nullptr
            || texture_width_ != frame.width
            || texture_height_ != frame.height
            || texture_format_ != frame.format
            || texture_wraps_d3d11_source_) {
            if (texture_ != nullptr) {
                SDL_DestroyTexture(texture_);
            }
            texture_ = SDL_CreateTexture(
                renderer(),
                sdl_pixel_format_for_frame(frame.format),
                SDL_TEXTUREACCESS_STREAMING,
                frame.width,
                frame.height);
            if (texture_ == nullptr) {
                throw_sdl_error("SDL_CreateTexture");
            }
            texture_width_ = frame.width;
            texture_height_ = frame.height;
            texture_format_ = frame.format;
            texture_wraps_d3d11_source_ = false;
            texture_wrapped_d3d11_source_ = nullptr;
        }

        update_pikvm_texture(texture_, frame);
    }

    PikvmViewOptions options_;
    PikvmViewOptions network_options_;
    std::shared_ptr<PikvmViewState> state_;
    PikvmKeyDownState key_down_{};
    PikvmMouseButtonDownState mouse_down_{};
    SDL_Texture* texture_ = nullptr;
    std::shared_ptr<PikvmD3D11Context> d3d11_context_;
    int texture_width_ = 0;
    int texture_height_ = 0;
    PikvmVideoPixelFormat texture_format_ = PikvmVideoPixelFormat::rgba32;
    bool texture_wraps_d3d11_source_ = false;
    bool d3d11_direct_wrap_disabled_ = false;
    ID3D11Texture2D* texture_wrapped_d3d11_source_ = nullptr;
    PikvmFrameLatencyBatch frame_latency_;
    PikvmClock::time_point last_frame_latency_log_ = PikvmClock::now();
    std::shared_ptr<const PikvmVideoFrame> pending_present_latency_frame_;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t last_mouse_motion_ticks_ = 0;
};

} // namespace

void run_pikvm_view(const PikvmViewOptions& options)
{
    try {
        PikvmView view(options);
        view.run();
    } catch (const UserError&) {
        throw;
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "pikvm view ui thread");
        throw;
    }
}

} // namespace hitsc
