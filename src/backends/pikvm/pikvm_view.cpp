#include "pikvm_view.hpp"

#include "diagnostics.hpp"
#include "errors.hpp"
#include "log.hpp"
#include "pikvm_events.hpp"
#include "pikvm_input.hpp"
#include "pikvm_session.hpp"
#include "pikvm_video.hpp"
#include "pikvm_video_hardware.hpp"
#include "view_base.hpp"
#include "view_input.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
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

namespace {

using PikvmClock = std::chrono::steady_clock;

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
    std::shared_ptr<PikvmVideoHardware> hardware;
};

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

PikvmRendererSetup create_pikvm_renderer(SDL_Window* window, const PikvmViewOptions& options)
{
    if (options.video_decode != PikvmVideoDecodeMode::software) {
        PikvmVideoHardwareRenderer setup =
            try_create_pikvm_video_hardware_renderer(window, options.login.verbose);
        if (setup.renderer != nullptr && setup.hardware) {
            return {setup.renderer, std::move(setup.hardware)};
        }
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        throw_sdl_error("SDL_CreateRenderer");
    }
    if (options.login.verbose) {
        log_info() << "SDL renderer selected for PiKVM"
                   << " name=" << SDL_GetRendererName(renderer)
                   << " hardware=no";
    }
    return {renderer, {}};
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
    case PikvmVideoPixelFormat::hardware_nv12:
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
    case PikvmVideoPixelFormat::hardware_nv12:
        throw std::runtime_error("hardware PiKVM frames must be uploaded with the hardware path");
    }
}

void set_pikvm_status(PikvmViewState& state, std::string status)
{
    std::lock_guard lock(state.control_mutex);
    state.status = std::move(status);
}

void store_pikvm_frame(PikvmViewState& state, PikvmVideoFrame frame)
{
    frame.timing.stored_at = PikvmClock::now();
    state.frames.publish(std::move(frame));
    state.push_render_event();
}

void release_pikvm_latest_frame(
    PikvmViewState& state,
    const std::shared_ptr<PikvmVideoHardware>& hardware)
{
    std::shared_ptr<const PikvmVideoFrame> frame = state.frames.clear();

    if (frame && hardware && frame->format == PikvmVideoPixelFormat::hardware_nv12) {
        auto lock = hardware->lock();
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
    const std::shared_ptr<PikvmVideoHardware>& hardware)
{
    if (texture == nullptr) {
        return;
    }

    std::unique_lock<std::recursive_mutex> hardware_lock;
    if (hardware) {
        hardware_lock = hardware->lock();
    }
    SDL_DestroyTexture(texture);
    texture = nullptr;
}

void reset_pikvm_texture_state(
    SDL_Texture*& texture,
    int& texture_width,
    int& texture_height,
    PikvmVideoPixelFormat& texture_format,
    bool& texture_wraps_hardware_source,
    const void*& texture_wrapped_hardware_source,
    const std::shared_ptr<PikvmVideoHardware>& hardware)
{
    destroy_pikvm_texture(texture, hardware);
    texture_width = 0;
    texture_height = 0;
    texture_format = PikvmVideoPixelFormat::rgba32;
    texture_wraps_hardware_source = false;
    texture_wrapped_hardware_source = nullptr;
}

void destroy_pikvm_renderer(
    SDL_Renderer*& renderer,
    const std::shared_ptr<PikvmVideoHardware>& hardware)
{
    if (renderer == nullptr) {
        return;
    }

    std::unique_lock<std::recursive_mutex> hardware_lock;
    if (hardware) {
        hardware_lock = hardware->lock();
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
    std::shared_ptr<PikvmVideoHardware> hardware,
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
            std::move(hardware),
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
    std::shared_ptr<PikvmVideoHardware> hardware,
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
            std::move(hardware),
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

class PikvmInputEncoder : public KvmInputEncoder {
public:
    explicit PikvmInputEncoder(PikvmViewState& state)
        : state_(state)
    {
    }

    bool accepts_button(std::uint8_t button) const override
    {
        return pikvm_mouse_button_from_sdl_button(button).has_value()
            && button < kPikvmMouseButtonSlots;
    }

    bool accepts_key(SDL_Scancode scancode) const override
    {
        return pikvm_key_code_from_sdl_scancode(scancode).has_value();
    }

    void encode_pointer(const PointerState& state, const PointerChange& change) override
    {
        const PikvmAbsoluteMousePosition position =
            make_pikvm_absolute_mouse_position(state.position.x, state.position.y);
        enqueue(make_pikvm_mouse_move_packet(position));

        if (change.kind == PointerChange::Kind::Button) {
            if (const auto button = pikvm_mouse_button_from_sdl_button(change.button)) {
                enqueue(make_pikvm_mouse_button_packet(*button, change.pressed));
            }
        } else if (change.kind == PointerChange::Kind::Wheel) {
            const int delta_x = change.wheel_x == 0.0f ? 0 : (change.wheel_x > 0.0f ? 5 : -5);
            const int delta_y = change.wheel_y == 0.0f ? 0 : (change.wheel_y > 0.0f ? 5 : -5);
            if (delta_x != 0 || delta_y != 0) {
                enqueue(make_pikvm_mouse_wheel_packet(delta_x, delta_y));
            }
        }
    }

    void encode_keyboard(const KeyboardState&, const KeyChange& change) override
    {
        for (const SDL_Scancode scancode : change.released) {
            if (const auto code = pikvm_key_code_from_sdl_scancode(scancode)) {
                enqueue(make_pikvm_key_packet(*code, false));
            }
        }
        for (const SDL_Scancode scancode : change.pressed) {
            if (const auto code = pikvm_key_code_from_sdl_scancode(scancode)) {
                enqueue(make_pikvm_key_packet(*code, true));
            }
        }
    }

private:
    void enqueue(std::vector<std::uint8_t> packet)
    {
        state_.input.enqueue(PikvmInputWork{std::move(packet)});
    }

    PikvmViewState& state_;
};

class PikvmView : public KvmViewBase {
public:
    explicit PikvmView(const PikvmViewOptions& options)
        : PikvmView(options, std::make_shared<PikvmViewState>())
    {
    }

private:
    PikvmView(const PikvmViewOptions& options, std::shared_ptr<PikvmViewState> state)
        : KvmViewBase(*state, options.login.base_url.host, options.login.host_id, "pikvm", [state] {
              state->input.clear();
          })
        , options_(options)
        , network_options_(options)
        , state_(std::move(state))
        , encoder_(*state_)
        , input_(encoder_, [this] { return frame_geometry(); })
    {
    }

    std::optional<FrameGeometry> frame_geometry() const
    {
        if (texture_width_ <= 0 || texture_height_ <= 0) {
            return std::nullopt;
        }
        return FrameGeometry{
            texture_width_,
            texture_height_,
            current_target_rect(texture_width_, texture_height_)};
    }

    SDL_Renderer* create_renderer(SDL_Window* window) override
    {
        PikvmRendererSetup renderer_setup = create_pikvm_renderer(window, options_);
        hardware_ = renderer_setup.hardware;
        if (!hardware_ && network_options_.video_decode == PikvmVideoDecodeMode::auto_select) {
            network_options_.video_decode = PikvmVideoDecodeMode::software;
        }
        return renderer_setup.renderer;
    }

    void destroy_renderer(SDL_Renderer* renderer) override
    {
        SDL_Renderer* renderer_to_destroy = renderer;
        destroy_pikvm_renderer(renderer_to_destroy, hardware_);
        hardware_.reset();
    }

    void start_network(KvmNetworkWorker& network) override
    {
        PikvmViewOptions network_options = network_options_;
        std::shared_ptr<PikvmViewState> state = state_;
        network.start([network_options, hardware = hardware_, state](
                          std::atomic_bool& stop_requested) {
            run_pikvm_network_session(network_options, hardware, *state, stop_requested);
        });
    }

    void before_sdl_cleanup() override
    {
        input_.reset();
        if (options_.login.vverbose) {
            maybe_log_pikvm_frame_latency(frame_latency_, last_frame_latency_log_, true);
        }
        pending_present_latency_frame_.reset();
        release_pikvm_latest_frame(*state_, hardware_);
        destroy_pikvm_texture(texture_, hardware_);
    }

    void reset_for_reconnect() override
    {
        input_.reset();
        pending_present_latency_frame_.reset();
        release_pikvm_latest_frame(*state_, hardware_);
        reset_pikvm_texture_state(
            texture_,
            texture_width_,
            texture_height_,
            texture_format_,
            texture_wraps_hardware_source_,
            texture_wrapped_hardware_source_,
            hardware_);
        last_sequence_ = 0;
        state_->video_decode_paused.store(false);
    }

    void on_close() override
    {
        input_.reset();
    }

    void on_minimized() override
    {
        state_->video_decode_paused.store(true);
        pending_present_latency_frame_.reset();
        release_pikvm_latest_frame(*state_, hardware_);
        reset_pikvm_texture_state(
            texture_,
            texture_width_,
            texture_height_,
            texture_format_,
            texture_wraps_hardware_source_,
            texture_wrapped_hardware_source_,
            hardware_);
        last_sequence_ = 0;
    }

    void on_restored() override
    {
        state_->video_decode_paused.store(false);
    }

    void on_focus_lost() override
    {
        input_.release_all_keys();
    }

    void handle_event(const SDL_Event& event, bool&) override
    {
        input_.handle_event(event);
    }

    void render_visible(bool& render_needed, bool& first_render) override
    {
        upload_latest_frame(render_needed);
        if (!render_needed || state_->video_decode_paused.load()) {
            return;
        }

        std::unique_lock<std::recursive_mutex> hardware_render_lock;
        if (hardware_) {
            hardware_render_lock = hardware_->lock();
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

    void upload_latest_frame(bool& render_needed)
    {
        const std::shared_ptr<const PikvmVideoFrame> frame =
            state_->video_decode_paused.load()
                ? nullptr
                : state_->frames.latest(last_sequence_);
        if (!frame) {
            return;
        }

        std::unique_lock<std::recursive_mutex> hardware_render_lock;
        if (hardware_) {
            hardware_render_lock = hardware_->lock();
        }

        last_sequence_ = frame->sequence;
        if (frame->format == PikvmVideoPixelFormat::hardware_nv12) {
            upload_hardware_frame(*frame);
        } else {
            upload_software_frame(*frame);
        }

        pending_present_latency_frame_ = frame;
        render_needed = true;
    }

    void upload_hardware_frame(const PikvmVideoFrame& frame)
    {
        if (!hardware_) {
            throw std::runtime_error("hardware PiKVM frame received without a hardware video backend");
        }

        const bool direct_wrap =
            !hardware_direct_wrap_disabled_ && hardware_->frame_can_wrap_direct(frame);
        if (direct_wrap) {
            try_wrap_hardware_frame(frame);
        }

        if (texture_ == nullptr || !texture_wraps_hardware_source_) {
            ensure_hardware_copy_texture(frame);
            hardware_->copy_frame_to_texture(texture_, frame);
        }
    }

    void try_wrap_hardware_frame(const PikvmVideoFrame& frame)
    {
        const void* source_id = hardware_->frame_source_id(frame);
        if (texture_ != nullptr
            && texture_width_ == frame.width
            && texture_height_ == frame.height
            && texture_format_ == frame.format
            && texture_wraps_hardware_source_
            && texture_wrapped_hardware_source_ == source_id) {
            return;
        }

        if (texture_ != nullptr) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }

        std::string wrap_error;
        texture_ = hardware_->try_create_wrapped_texture(renderer(), frame, wrap_error);
        if (texture_ != nullptr) {
            texture_width_ = frame.width;
            texture_height_ = frame.height;
            texture_format_ = frame.format;
            texture_wraps_hardware_source_ = true;
            texture_wrapped_hardware_source_ = source_id;
            if (options_.login.verbose) {
                log_info() << "wrapped PiKVM hardware video texture directly"
                           << " backend=" << hardware_->name();
            }
            return;
        }

        hardware_direct_wrap_disabled_ = true;
        texture_width_ = 0;
        texture_height_ = 0;
        texture_format_ = PikvmVideoPixelFormat::rgba32;
        texture_wraps_hardware_source_ = false;
        texture_wrapped_hardware_source_ = nullptr;
        if (options_.login.verbose) {
            log_warning() << "direct hardware texture wrap failed; using GPU copy"
                          << " backend=" << hardware_->name()
                          << " error=" << wrap_error;
        }
    }

    void ensure_hardware_copy_texture(const PikvmVideoFrame& frame)
    {
        if (texture_ != nullptr
            && texture_width_ == frame.width
            && texture_height_ == frame.height
            && texture_format_ == frame.format
            && !texture_wraps_hardware_source_) {
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
            throw_sdl_error("SDL_CreateTexture(hardware NV12)");
        }
        if (!hardware_->texture_can_receive_copy(texture_)) {
            throw std::runtime_error("SDL NV12 texture did not expose a hardware video resource");
        }
        texture_width_ = frame.width;
        texture_height_ = frame.height;
        texture_format_ = frame.format;
        texture_wraps_hardware_source_ = false;
        texture_wrapped_hardware_source_ = nullptr;
    }

    void upload_software_frame(const PikvmVideoFrame& frame)
    {
        if (texture_ == nullptr
            || texture_width_ != frame.width
            || texture_height_ != frame.height
            || texture_format_ != frame.format
            || texture_wraps_hardware_source_) {
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
            texture_wraps_hardware_source_ = false;
            texture_wrapped_hardware_source_ = nullptr;
        }

        update_pikvm_texture(texture_, frame);
    }

    PikvmViewOptions options_;
    PikvmViewOptions network_options_;
    std::shared_ptr<PikvmViewState> state_;
    PikvmInputEncoder encoder_;
    KvmInputController input_;
    SDL_Texture* texture_ = nullptr;
    std::shared_ptr<PikvmVideoHardware> hardware_;
    int texture_width_ = 0;
    int texture_height_ = 0;
    PikvmVideoPixelFormat texture_format_ = PikvmVideoPixelFormat::rgba32;
    bool texture_wraps_hardware_source_ = false;
    bool hardware_direct_wrap_disabled_ = false;
    const void* texture_wrapped_hardware_source_ = nullptr;
    PikvmFrameLatencyBatch frame_latency_;
    PikvmClock::time_point last_frame_latency_log_ = PikvmClock::now();
    std::shared_ptr<const PikvmVideoFrame> pending_present_latency_frame_;
    std::uint64_t last_sequence_ = 0;
};

} // namespace

void run_pikvm_view(const PikvmViewOptions& options, const ViewWindow* handoff)
{
    try {
        PikvmView view(options);
        if (handoff != nullptr) {
            view.adopt_sdl(*handoff);
        }
        view.run();
    } catch (const UserError&) {
        throw;
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "pikvm view ui thread");
        throw;
    }
}

} // namespace hitsc
