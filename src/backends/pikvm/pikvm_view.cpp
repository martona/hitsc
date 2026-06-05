#include "pikvm_view.hpp"

#include "diagnostics.hpp"
#include "errors.hpp"
#include "gui/viewer/qt_viewer_host.hpp"
#include "log.hpp"
#include "pikvm_events.hpp"
#include "pikvm_input.hpp"
#include "pikvm_session.hpp"
#include "pikvm_video.hpp"
#include "pikvm_video_hardware.hpp"
#include "power_rest_worker.hpp"
#include "view_base.hpp"
#include "view_input.hpp"

#include <QImage>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

void set_pikvm_status(PikvmViewState& state, std::string status)
{
    std::lock_guard lock(state.control_mutex);
    state.status = std::move(status);
}

void store_pikvm_frame(PikvmViewState& state, PikvmVideoFrame frame)
{
    frame.timing.stored_at = PikvmClock::now();
    state.frames.publish(std::move(frame));
}

// PiKVM ATX request path for a power action. We match the kvmd WEB UI exactly: every
// action is an UNCONDITIONAL /api/atx/click (physical front-panel button emulation),
// never the state-aware /api/atx/power -- which silently no-ops off/off_hard/
// reset_hard (HTTP 200, no effect) unless kvmd reads the host as ON, the reason
// off/reset "did nothing" here while the web UI worked. A short power click toggles,
// so On and Graceful both map to button=power; the popup's state-based enable/disable
// keeps On usable only when off and Graceful/off/reset only when on. power_long =
// forced off (long hold), reset = reset.
std::string pikvm_atx_target(PowerAction action)
{
    switch (action) {
    case PowerAction::On:          return "/api/atx/click?button=power";
    case PowerAction::OffGraceful: return "/api/atx/click?button=power";
    case PowerAction::OffHard:     return "/api/atx/click?button=power_long";
    case PowerAction::Reset:       return "/api/atx/click?button=reset";
    }
    return "/api/atx/click?button=power";
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
                [&](bool power_on) {
                    state.power.publish_state(power_on ? PowerState::On : PowerState::Off);
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

    // Diagnostic probe of the host ATX state (logs enabled / leds.power so a stuck or
    // unwired power-LED is visible). Runs on this thread alone -- no power worker yet,
    // so it can't race web.request(). The live atx events drive the actual indicator.
    try {
        auto atx = session.web.request(http::verb::get, "/api/atx", {}, {});
        log_info() << "pikvm /api/atx -> HTTP " << atx.result_int()
                   << " body=" << decode_response_body(atx);
    } catch (const std::exception& ex) {
        log_warning() << "pikvm /api/atx probe failed: " << ex.what();
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

    // Power control on a dedicated, cancelable REST worker (reuses this login) so a
    // power POST never stalls the control/video websockets. Joined before logout.
    auto power_worker = std::make_shared<PowerRestWorker>(
        session.web,
        [](PowerAction action) {
            PowerRequestSpec spec;
            spec.method = http::verb::post;
            spec.target = pikvm_atx_target(action);
            return spec;
        },
        [&state](PowerOutcome outcome) {
            state.power.publish_outcome(std::move(outcome));
        });
    std::weak_ptr<PowerRestWorker> weak_power_worker = power_worker;
    state.power.install([weak_power_worker](PowerAction action) {
        if (auto worker = weak_power_worker.lock()) {
            worker->submit(action);
        }
    });

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
        state.power.clear();
        state.set_force_close({});
        throw;
    }

    stop_handles->set_control({});
    stop_handles->set_video({});
    state.input.clear();
    state.power.clear();
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

    bool accepts_button(KvmMouseButton button) const override
    {
        return pikvm_mouse_button_from_button(button).has_value()
            && static_cast<std::size_t>(button) < kPikvmMouseButtonSlots;
    }

    bool accepts_key(KvmScancode scancode) const override
    {
        return pikvm_key_code_from_scancode(scancode).has_value();
    }

    void encode_pointer(const PointerState& state, const PointerChange& change) override
    {
        const PikvmAbsoluteMousePosition position =
            make_pikvm_absolute_mouse_position(state.position.x, state.position.y);
        enqueue(make_pikvm_mouse_move_packet(position));

        if (change.kind == PointerChange::Kind::Button) {
            if (const auto button = pikvm_mouse_button_from_button(change.button)) {
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
        for (const KvmScancode scancode : change.released) {
            if (const auto code = pikvm_key_code_from_scancode(scancode)) {
                enqueue(make_pikvm_key_packet(*code, false));
            }
        }
        for (const KvmScancode scancode : change.pressed) {
            if (const auto code = pikvm_key_code_from_scancode(scancode)) {
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

    ~PikvmView() override
    {
        if (hosted_sws_ != nullptr) {
            sws_freeContext(hosted_sws_);
        }
    }

private:
    PikvmView(const PikvmViewOptions& options, std::shared_ptr<PikvmViewState> state)
        : KvmViewBase(*state, options.login.base_url.host, "pikvm", [state] {
              state->input.clear();
              state->power.clear();
          })
        , options_(options)
        , network_options_(options)
        , state_(std::move(state))
        , encoder_(*state_)
        , input_(encoder_, [] { return std::optional<FrameGeometry>{}; })
        , power_controller_(default_bmc_power_caps(), state_->power)
    {
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

    void set_rhi_d3d11_device(void* d3d11_device) override
    {
        // Bind FFmpeg D3D11VA decode to QRhi's device for zero-copy display. The
        // decoder still falls back to software internally if hardware init fails;
        // honor an explicit software request by not creating the adapter at all.
        if (options_.video_decode == PikvmVideoDecodeMode::software) {
            return;
        }
        hardware_ = make_pikvm_d3d11_hardware(
            static_cast<ID3D11Device*>(d3d11_device), options_.login.verbose);
    }

    void reset_for_reconnect() override
    {
        input_.reset();
        state_->frames.clear();
        hosted_last_sequence_ = 0;
        hosted_frame_ = QImage();
        hosted_hw_frame_.reset();
        state_->video_decode_paused.store(false);
    }

    void on_close() override
    {
        input_.reset();
    }

    void on_minimized() override
    {
        state_->video_decode_paused.store(true);
        state_->frames.clear();
    }

    void on_restored() override
    {
        state_->video_decode_paused.store(false);
    }

    void on_focus_lost() override
    {
        input_.release_all_keys();
    }

    KvmInputController* hosted_input_controller() override
    {
        return &input_;
    }

    PowerController* power_controller() override
    {
        return &power_controller_;
    }

    std::optional<std::pair<int, int>> hosted_input_resolution() override
    {
        // Fallback so pointer input maps (and can wake a sleeping host) even if no
        // frame has arrived. Normally pikvm's retained last-frame dims cover this;
        // this is the belt-and-suspenders default. Real frame dims override it.
        return std::make_pair(1920, 1080);
    }

    std::optional<SoftwareFrame> latest_frame() override
    {
        const std::shared_ptr<const PikvmVideoFrame> frame =
            state_->frames.latest(hosted_last_sequence_);
        if (frame && frame->format != PikvmVideoPixelFormat::hardware_nv12) {
            hosted_last_sequence_ = frame->sequence;
            if (std::optional<QImage> image = convert_hosted_frame(*frame)) {
                hosted_frame_ = std::move(*image);
                frame_presented(frame->width, frame->height);
            }
        }
        if (hosted_frame_.isNull()) {
            return std::nullopt;
        }
        // pikvm has no separate hardware-cursor overlay (the MJPEG fallback bakes
        // the cursor into the frame; the normal h264 path uses latest_hardware_frame).
        SoftwareFrame out;
        out.base = hosted_frame_;
        return out;
    }

    std::optional<HardwareVideoFrame> latest_hardware_frame() override
    {
        if (!hardware_) {
            return std::nullopt;
        }
        const std::shared_ptr<const PikvmVideoFrame> frame =
            state_->frames.latest(hosted_last_sequence_);
        if (frame && frame->format == PikvmVideoPixelFormat::hardware_nv12) {
            hosted_last_sequence_ = frame->sequence;
            hosted_hw_frame_ = frame;
            frame_presented(frame->width, frame->height);
        }
        if (!hosted_hw_frame_) {
            return std::nullopt;  // software session, or no frame yet
        }
        HardwareVideoFrame hw;
        hw.texture = hardware_->frame_texture(*hosted_hw_frame_);
        hw.array_slice = hardware_->frame_array_slice(*hosted_hw_frame_);
        hw.width = hosted_hw_frame_->width;
        hw.height = hosted_hw_frame_->height;
        hw.lock = hardware_->lock_handle();
        hw.keepalive = hosted_hw_frame_->owner;
        return hw;
    }

    std::optional<std::pair<int, int>> latest_frame_size() override
    {
        if (const std::shared_ptr<const PikvmVideoFrame> frame = state_->frames.latest(0)) {
            return std::make_pair(frame->width, frame->height);
        }
        if (!hosted_frame_.isNull()) {
            return std::make_pair(hosted_frame_.width(), hosted_frame_.height());
        }
        return std::nullopt;
    }

    // Software-decode frame -> RGBA QImage for the Qt surface. On the software
    // path (no D3D11 device handed to the decoder), frames are i420 / nv12
    // (swscale to RGBA) or already rgba32 (copy). hardware_nv12 never
    // reaches here. Published frames are immutable (each owns its AVFrame), so no
    // lock is needed. The GPU/zero-copy path is commit 4.
    std::optional<QImage> convert_hosted_frame(const PikvmVideoFrame& frame)
    {
        if (frame.width <= 0 || frame.height <= 0) {
            return std::nullopt;
        }

        if (frame.format == PikvmVideoPixelFormat::rgba32) {
            QImage image(frame.width, frame.height, QImage::Format_RGBA8888);
            const int src_pitch = frame.pitches[0] > 0 ? frame.pitches[0] : frame.width * 4;
            const std::uint8_t* src =
                frame.planes[0] != nullptr ? frame.planes[0] : frame.rgba.data();
            for (int y = 0; y < frame.height; ++y) {
                std::memcpy(
                    image.scanLine(y),
                    src + static_cast<std::size_t>(y) * static_cast<std::size_t>(src_pitch),
                    static_cast<std::size_t>(frame.width) * 4U);
            }
            return image;
        }

        AVPixelFormat source_format = AV_PIX_FMT_NONE;
        switch (frame.format) {
        case PikvmVideoPixelFormat::i420:
            source_format = AV_PIX_FMT_YUV420P;
            break;
        case PikvmVideoPixelFormat::nv12:
            source_format = AV_PIX_FMT_NV12;
            break;
        default:
            return std::nullopt;
        }

        hosted_sws_ = sws_getCachedContext(
            hosted_sws_,
            frame.width,
            frame.height,
            source_format,
            frame.width,
            frame.height,
            AV_PIX_FMT_RGBA,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (hosted_sws_ == nullptr) {
            return std::nullopt;
        }

        // Drive swscale's YUV->RGB matrix from the stream's colorspace/range
        // instead of letting it guess, so software-decoded pikvm video matches the
        // source (and the hardware NV12 shader). Unspecified -> BT.709 for HD /
        // BT.601 for SD, the usual heuristic.
        int source_cs = SWS_CS_ITU601;
        switch (frame.colorspace) {
        case AVCOL_SPC_BT709:
            source_cs = SWS_CS_ITU709;
            break;
        case AVCOL_SPC_SMPTE240M:
            source_cs = SWS_CS_SMPTE240M;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            source_cs = SWS_CS_BT2020;
            break;
        case AVCOL_SPC_FCC:
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            source_cs = SWS_CS_ITU601;
            break;
        default:
            source_cs = frame.height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
            break;
        }
        const int source_range = frame.color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        sws_setColorspaceDetails(
            hosted_sws_,
            sws_getCoefficients(source_cs), source_range,
            sws_getCoefficients(SWS_CS_DEFAULT), 1,  // RGBA output is full-range
            0, 1 << 16, 1 << 16);                    // neutral brightness/contrast/saturation

        QImage image(frame.width, frame.height, QImage::Format_RGBA8888);
        std::uint8_t* destination_data[4] = {image.bits(), nullptr, nullptr, nullptr};
        int destination_linesize[4] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
        const int scaled = sws_scale(
            hosted_sws_,
            frame.planes.data(),
            frame.pitches.data(),
            0,
            frame.height,
            destination_data,
            destination_linesize);
        if (scaled != frame.height) {
            return std::nullopt;
        }
        return image;
    }

    PikvmViewOptions options_;
    PikvmViewOptions network_options_;
    std::shared_ptr<PikvmViewState> state_;
    PikvmInputEncoder encoder_;
    KvmInputController input_;
    ViewPowerController power_controller_;
    SwsContext* hosted_sws_ = nullptr;
    QImage hosted_frame_;
    std::uint64_t hosted_last_sequence_ = 0;
    std::shared_ptr<PikvmVideoHardware> hardware_;
    std::shared_ptr<const PikvmVideoFrame> hosted_hw_frame_;
};

} // namespace

std::unique_ptr<KvmViewBase> make_pikvm_view(const PikvmViewOptions& options)
{
    return std::make_unique<PikvmView>(options);
}

void run_pikvm_view(const PikvmViewOptions& options)
{
    try {
        run_viewer({options.login.base_url.host, options.login.host_id}, [options](ViewerHost& host) {
            host.attach_view(make_pikvm_view(options));
        });
    } catch (const UserError&) {
        throw;
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "pikvm view ui thread");
        throw;
    }
}

} // namespace hitsc
