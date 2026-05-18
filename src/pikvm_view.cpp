#include "pikvm_view.hpp"

#include "diagnostics.hpp"
#include "log.hpp"
#include "pikvm_events.hpp"
#include "pikvm_input.hpp"
#include "pikvm_session.hpp"
#include "pikvm_video.hpp"

#include <SDL3/SDL.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cwchar>
#include <cmath>
#include <cstdint>
#include <deque>
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
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using Microsoft::WRL::ComPtr;

namespace {

using PikvmClock = std::chrono::steady_clock;

constexpr std::uint64_t kPikvmMouseMotionIntervalMilliseconds = 8;
constexpr std::chrono::milliseconds kPikvmInputStopGrace{250};
constexpr std::size_t kPikvmMouseButtonSlots = 8;

struct PikvmViewState {
    std::mutex frame_mutex;
    std::mutex control_mutex;
    std::shared_ptr<const PikvmVideoFrame> frame;
    std::atomic_uint32_t frame_event_type{0};
    std::atomic_bool frame_event_pending{false};
    std::uint64_t frame_sequence = 0;
    std::string status = "starting";
    std::exception_ptr exception;
    std::function<void()> force_close;
    std::function<void(PikvmInputWork)> input_sink;
    std::deque<PikvmInputWork> pending_input;
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

bool queue_pikvm_input_packet(
    PikvmViewState& state,
    std::vector<std::uint8_t> packet,
    bool coalesce_mouse_motion,
    PikvmClock::time_point ui_event_at = PikvmClock::now())
{
    PikvmInputWork work;
    work.packet = std::move(packet);
    work.coalesce_mouse_motion = coalesce_mouse_motion;
    work.timing.ui_event_at = ui_event_at;
    work.timing.enqueued_at = PikvmClock::now();

    std::function<void(PikvmInputWork)> input_sink;
    {
        std::lock_guard lock(state.control_mutex);
        input_sink = state.input_sink;
        if (!input_sink && coalesce_mouse_motion &&
            !state.pending_input.empty() &&
            is_pikvm_mouse_move_packet(state.pending_input.back().packet)) {
            state.pending_input.back() = std::move(work);
            return true;
        }
        if (!input_sink) {
            state.pending_input.push_back(std::move(work));
            return true;
        }
    }

    input_sink(std::move(work));
    return true;
}

void install_pikvm_input_sink(
    PikvmViewState& state,
    std::function<void(PikvmInputWork)> input_sink)
{
    std::deque<PikvmInputWork> pending;
    {
        std::lock_guard lock(state.control_mutex);
        state.input_sink = input_sink;
        pending.swap(state.pending_input);
    }

    for (PikvmInputWork& work : pending) {
        input_sink(std::move(work));
    }
}

void clear_pikvm_input_sink(PikvmViewState& state)
{
    std::lock_guard lock(state.control_mutex);
    state.input_sink = {};
    state.pending_input.clear();
}

void queue_pikvm_key_event(
    PikvmViewState& state,
    std::string_view code,
    bool pressed,
    bool verbose,
    PikvmClock::time_point ui_event_at = PikvmClock::now())
{
    (void)verbose;
    queue_pikvm_input_packet(state, make_pikvm_key_packet(code, pressed), false, ui_event_at);
}

void queue_pikvm_mouse_button_event(
    PikvmViewState& state,
    std::string_view button,
    bool pressed,
    bool verbose,
    PikvmClock::time_point ui_event_at = PikvmClock::now())
{
    (void)verbose;
    queue_pikvm_input_packet(state, make_pikvm_mouse_button_packet(button, pressed), false, ui_event_at);
}

void queue_pikvm_mouse_move_event(
    PikvmViewState& state,
    const PikvmAbsoluteMousePosition& position,
    bool coalesce_mouse_motion,
    PikvmClock::time_point ui_event_at = PikvmClock::now())
{
    queue_pikvm_input_packet(
        state,
        make_pikvm_mouse_move_packet(position),
        coalesce_mouse_motion,
        ui_event_at);
}

void queue_pikvm_mouse_wheel_event(
    PikvmViewState& state,
    int delta_x,
    int delta_y,
    bool verbose,
    PikvmClock::time_point ui_event_at = PikvmClock::now())
{
    (void)verbose;
    queue_pikvm_input_packet(
        state,
        make_pikvm_mouse_wheel_packet(delta_x, delta_y),
        false,
        ui_event_at);
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
    PikvmKeyDownState& key_down,
    bool verbose)
{
    for (std::size_t scancode = 0; scancode < key_down.size(); ++scancode) {
        if (!key_down[scancode]) {
            continue;
        }
        key_down[scancode] = false;
        const auto code = pikvm_key_code_from_sdl_scancode(static_cast<SDL_Scancode>(scancode));
        if (code) {
            queue_pikvm_key_event(state, *code, false, verbose);
        }
    }
}

void release_all_pikvm_mouse_buttons(
    PikvmViewState& state,
    PikvmMouseButtonDownState& mouse_down,
    bool verbose)
{
    for (std::size_t slot = 0; slot < mouse_down.size(); ++slot) {
        if (!mouse_down[slot]) {
            continue;
        }
        mouse_down[slot] = false;
        const auto button = pikvm_mouse_button_from_sdl_button(static_cast<std::uint8_t>(slot));
        if (button) {
            queue_pikvm_mouse_button_event(state, *button, false, verbose);
        }
    }
    SDL_CaptureMouse(false);
}

void release_all_pikvm_input(
    PikvmViewState& state,
    PikvmKeyDownState& key_down,
    PikvmMouseButtonDownState& mouse_down,
    bool verbose)
{
    release_all_pikvm_keys(state, key_down, verbose);
    release_all_pikvm_mouse_buttons(state, mouse_down, verbose);
}

void store_pikvm_frame(PikvmViewState& state, PikvmVideoFrame frame)
{
    frame.timing.stored_at = PikvmClock::now();
    {
        std::lock_guard lock(state.frame_mutex);
        frame.sequence = ++state.frame_sequence;
        state.frame = std::make_shared<PikvmVideoFrame>(std::move(frame));
    }

    const auto frame_event_type = static_cast<Uint32>(state.frame_event_type.load());
    if (frame_event_type != 0 && !state.frame_event_pending.exchange(true)) {
        SDL_Event event{};
        event.type = frame_event_type;
        if (!SDL_PushEvent(&event)) {
            state.frame_event_pending.store(false);
        }
    }
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

struct PikvmControlStopState {
    std::weak_ptr<PikvmWebSocket> ws;
    std::weak_ptr<PikvmEventSession> session;
    std::atomic_bool* stop_requested = nullptr;
};

struct PikvmVideoStopState {
    std::weak_ptr<PikvmWebSocket> ws;
    std::atomic_bool* stop_requested = nullptr;
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
    } else {
        force_close_weak_socket(stop_state->ws);
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
    force_close_weak_socket(stop_state->ws);
}

void run_pikvm_control_worker(
    PikvmViewOptions options,
    CookieJar cookies,
    PikvmViewState& state,
    std::atomic_bool& stop_requested,
    const std::shared_ptr<PikvmNetworkStopHandles>& stop_handles,
    std::function<void(std::exception_ptr)> on_error)
{
    auto stop_state = std::make_shared<PikvmControlStopState>();
    stop_state->stop_requested = &stop_requested;

    try {
        asio::io_context io;
        ssl::context tls_context(ssl::context::tls_client);

        set_pikvm_status(state, "connecting control websocket");
        std::shared_ptr<PikvmWebSocket> event_ws =
            connect_pikvm_websocket(
                io,
                tls_context,
                options.login,
                cookies,
                options.idle_timeout_seconds,
                "/api/ws?stream=1");
        stop_state->ws = event_ws;
        stop_handles->set_control([stop_state] {
            request_pikvm_control_stop(stop_state);
        });

        if (stop_requested.load()) {
            request_pikvm_control_stop(stop_state);
        } else {
            std::shared_ptr<PikvmEventSession> event_session =
                start_pikvm_event_session(event_ws, options, stop_requested, on_error);
            stop_state->session = event_session;
            install_pikvm_input_sink(
                state,
                [event_session](PikvmInputWork work) {
                    queue_pikvm_event_input(event_session, std::move(work));
                });
            set_pikvm_status(state, "control websocket connected");
        }

        io.run();
    } catch (...) {
        if (!stop_requested.load()) {
            on_error(std::current_exception());
        }
    }

    clear_pikvm_input_sink(state);
    stop_handles->set_control({});
}

void run_pikvm_video_worker(
    PikvmViewOptions options,
    CookieJar cookies,
    std::shared_ptr<PikvmD3D11Context> d3d11_context,
    PikvmViewState& state,
    std::atomic_bool& stop_requested,
    const std::shared_ptr<PikvmNetworkStopHandles>& stop_handles,
    std::function<void(std::exception_ptr)> on_error)
{
    auto stop_state = std::make_shared<PikvmVideoStopState>();
    stop_state->stop_requested = &stop_requested;

    try {
        asio::io_context io;
        ssl::context tls_context(ssl::context::tls_client);

        set_pikvm_status(state, "connecting video websocket");
        std::shared_ptr<PikvmWebSocket> video_ws =
            connect_pikvm_websocket(
                io,
                tls_context,
                options.login,
                cookies,
                options.idle_timeout_seconds,
                "/api/media/ws");
        stop_state->ws = video_ws;
        stop_handles->set_video([stop_state] {
            request_pikvm_video_stop(stop_state);
        });

        if (stop_requested.load()) {
            request_pikvm_video_stop(stop_state);
        } else {
            start_pikvm_video_stream(
                video_ws,
                options,
                std::move(d3d11_context),
                stop_requested,
                [&](PikvmVideoFrame frame) {
                    store_pikvm_frame(state, std::move(frame));
                },
                on_error);
            set_pikvm_status(state, "connected");
        }

        io.run();
    } catch (...) {
        if (!stop_requested.load()) {
            on_error(std::current_exception());
        }
    }

    stop_handles->set_video({});
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

    auto stop_handles = std::make_shared<PikvmNetworkStopHandles>();
    auto request_stop = [stop_handles, &stop_requested] {
        stop_requested.store(true);
        stop_handles->stop_all();
    };
    set_pikvm_force_close(state, request_stop);

    auto on_error = [&](std::exception_ptr exception) {
        set_pikvm_exception(state, exception);
        request_stop();
    };

    std::thread control_thread;
    std::thread video_thread;
    try {
        control_thread = std::thread(
            run_pikvm_control_worker,
            options,
            session.cookies,
            std::ref(state),
            std::ref(stop_requested),
            stop_handles,
            on_error);
        video_thread = std::thread(
            run_pikvm_video_worker,
            options,
            session.cookies,
            std::move(d3d11_context),
            std::ref(state),
            std::ref(stop_requested),
            stop_handles,
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
        clear_pikvm_input_sink(state);
        set_pikvm_force_close(state, {});
        throw;
    }

    clear_pikvm_input_sink(state);
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
    PikvmKeyDownState key_down{};
    PikvmMouseButtonDownState mouse_down{};

    try {
        const Uint32 frame_event_type = SDL_RegisterEvents(1);
        if (frame_event_type == 0) {
            throw_sdl_error("SDL_RegisterEvents");
        }
        state->frame_event_type.store(frame_event_type);

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
            clear_pikvm_input_sink(*state);
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
        PikvmFrameLatencyBatch frame_latency;
        PikvmClock::time_point last_frame_latency_log = PikvmClock::now();
        std::shared_ptr<const PikvmVideoFrame> pending_present_latency_frame;
        std::uint64_t last_mouse_motion_ticks = 0;

        while (running) {
            bool render_needed = first_render;
            SDL_Event event{};
            bool have_event = SDL_WaitEventTimeout(&event, 16);
            while (have_event) {
                const auto ui_event_at = PikvmClock::now();
                if (event.type == SDL_EVENT_QUIT ||
                    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    if (!close_event_logged) {
                        close_event_logged = true;
                        log_info() << "pikvm window close event"
                                   << " type=" << event.type;
                    }
                    release_all_pikvm_input(*state, key_down, mouse_down, options.login.verbose);
                    running = false;
                } else if (event.type == state->frame_event_type.load()) {
                    state->frame_event_pending.store(false);
                    render_needed = true;
                } else if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                           event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                           event.type == SDL_EVENT_WINDOW_EXPOSED) {
                    render_needed = true;
                } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    release_all_pikvm_keys(*state, key_down, options.login.verbose);
                } else if (event.type == SDL_EVENT_KEY_DOWN ||
                           event.type == SDL_EVENT_KEY_UP) {
                    if (!(event.type == SDL_EVENT_KEY_DOWN && event.key.repeat)) {
                        const std::optional<std::string_view> code =
                            pikvm_key_code_from_sdl_scancode(event.key.scancode);
                        const auto scancode = static_cast<std::size_t>(event.key.scancode);
                        if (!code || scancode >= key_down.size()) {
                            if (options.login.verbose) {
                                log_info() << "ignored PiKVM key"
                                           << " scancode=" << event.key.scancode
                                           << " key=" << event.key.key;
                            }
                        } else {
                            const bool down = event.type == SDL_EVENT_KEY_DOWN;
                            if (key_down[scancode] != down) {
                                key_down[scancode] = down;
                                queue_pikvm_key_event(
                                    *state,
                                    *code,
                                    down,
                                    options.login.verbose,
                                    ui_event_at);
                            }
                        }
                    }
                } else if (texture_width > 0 && texture_height > 0 &&
                           (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                            event.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
                    const std::optional<std::string_view> button =
                        pikvm_mouse_button_from_sdl_button(event.button.button);
                    std::size_t button_slot = 0;
                    if (button && pikvm_mouse_button_slot(event.button.button, button_slot)) {
                        const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                        const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                        const bool drag_active = any_pikvm_mouse_button_down(mouse_down);
                        const std::optional<PikvmAbsoluteMousePosition> position = pikvm_mouse_position(
                            event.button.x,
                            event.button.y,
                            target,
                            drag_active || !down);

                        if (down && !position) {
                            have_event = SDL_PollEvent(&event);
                            continue;
                        }
                        if (position) {
                            queue_pikvm_mouse_move_event(*state, *position, false, ui_event_at);
                        }

                        if (mouse_down[button_slot] != down) {
                            mouse_down[button_slot] = down;
                            queue_pikvm_mouse_button_event(
                                *state,
                                *button,
                                down,
                                options.login.verbose,
                                ui_event_at);
                        }
                        SDL_CaptureMouse(any_pikvm_mouse_button_down(mouse_down));
                    } else if (options.login.verbose) {
                        log_info() << "ignored PiKVM mouse button"
                                   << " button=" << static_cast<int>(event.button.button);
                    }
                } else if (texture_width > 0 && texture_height > 0 &&
                           event.type == SDL_EVENT_MOUSE_MOTION) {
                    const bool drag_active = any_pikvm_mouse_button_down(mouse_down);
                    const std::uint64_t ticks = SDL_GetTicks();
                    const bool throttled =
                        !drag_active
                        && ticks - last_mouse_motion_ticks < kPikvmMouseMotionIntervalMilliseconds;
                    if (!throttled) {
                        const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                        const std::optional<PikvmAbsoluteMousePosition> position = pikvm_mouse_position(
                            event.motion.x,
                            event.motion.y,
                            target,
                            drag_active);
                        if (position) {
                            queue_pikvm_mouse_move_event(
                                *state,
                                *position,
                                !drag_active,
                                ui_event_at);
                            last_mouse_motion_ticks = ticks;
                        }
                    }
                } else if (texture_width > 0 && texture_height > 0 &&
                           event.type == SDL_EVENT_MOUSE_WHEEL) {
                    const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                    const std::optional<PikvmAbsoluteMousePosition> position = pikvm_mouse_position(
                        event.wheel.mouse_x,
                        event.wheel.mouse_y,
                        target,
                        any_pikvm_mouse_button_down(mouse_down));
                    if (position) {
                        queue_pikvm_mouse_move_event(*state, *position, false, ui_event_at);
                        const float x = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
                            ? -event.wheel.x
                            : event.wheel.x;
                        const float y = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
                            ? -event.wheel.y
                            : event.wheel.y;
                        const int delta_x = x == 0.0f ? 0 : (x > 0.0f ? 5 : -5);
                        const int delta_y = y == 0.0f ? 0 : (y > 0.0f ? 5 : -5);
                        if (delta_x != 0 || delta_y != 0) {
                            queue_pikvm_mouse_wheel_event(
                                *state,
                                delta_x,
                                delta_y,
                                options.login.verbose,
                                ui_event_at);
                        }
                    }
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

                pending_present_latency_frame = frame;
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
                if (options.login.verbose && pending_present_latency_frame) {
                    add_pikvm_frame_latency(
                        frame_latency,
                        *pending_present_latency_frame,
                        PikvmClock::now());
                    pending_present_latency_frame.reset();
                    maybe_log_pikvm_frame_latency(frame_latency, last_frame_latency_log, false);
                }
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
        if (options.login.verbose) {
            maybe_log_pikvm_frame_latency(frame_latency, last_frame_latency_log, true);
        }
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "pikvm view ui thread");
        release_all_pikvm_input(*state, key_down, mouse_down, options.login.verbose);
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

    release_all_pikvm_input(*state, key_down, mouse_down, options.login.verbose);
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
