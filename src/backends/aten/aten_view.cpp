#include "aten_view.hpp"

#include "backends/aspeed/aspeed_view.hpp"
#include "aten_media_session.hpp"
#include "aten_network.hpp"
#include "aten_protocol.hpp"
#include "diagnostics.hpp"
#include "gui/viewer/qt_viewer_host.hpp"
#include "hardware_cursor.hpp"
#include "view_input.hpp"
#include "virtual_media/virtual_media.hpp"

#include <QImage>
#include <QRect>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hitsc {

extern std::atomic_bool g_aten_full_framebuffer_refresh_requested;

namespace {

std::optional<std::uint32_t> aten_keyboard_usage_from_scancode(KvmScancode scancode)
{
    const auto usage = static_cast<int>(scancode);
    if ((usage >= static_cast<int>(KvmScancode::A) && usage <= static_cast<int>(KvmScancode::APPLICATION)) ||
        (usage >= static_cast<int>(KvmScancode::KP_EQUALS) && usage <= static_cast<int>(KvmScancode::RGUI))) {
        return static_cast<std::uint32_t>(usage);
    }

    return std::nullopt;
}

std::uint8_t aten_button_mask(std::uint32_t buttons)
{
    std::uint8_t mask = 0;
    if (buttons & (1u << static_cast<unsigned>(KvmMouseButton::LEFT))) {
        mask |= 1;
    }
    if (buttons & (1u << static_cast<unsigned>(KvmMouseButton::MIDDLE))) {
        mask |= 2;
    }
    if (buttons & (1u << static_cast<unsigned>(KvmMouseButton::RIGHT))) {
        mask |= 4;
    }
    return mask;
}

class AtenInputEncoder : public KvmInputEncoder {
public:
    explicit AtenInputEncoder(AtenViewState& state)
        : state_(state)
    {
    }

    bool accepts_button(KvmMouseButton button) const override
    {
        return button == KvmMouseButton::LEFT || button == KvmMouseButton::MIDDLE || button == KvmMouseButton::RIGHT;
    }

    bool accepts_key(KvmScancode scancode) const override
    {
        return aten_keyboard_usage_from_scancode(scancode).has_value();
    }

    void encode_pointer(const PointerState& state, const PointerChange& change) override
    {
        const FramePixel pixel = to_frame_pixel(state.position, state.frame_width, state.frame_height);

        if (change.kind == PointerChange::Kind::Wheel) {
            if (change.wheel_y == 0.0f) {
                return;
            }
            const std::uint8_t wheel_mask = change.wheel_y > 0.0f ? 8U : 16U;
            state_.input.enqueue(make_aten_pointer_event(pixel.x, pixel.y, wheel_mask));
            state_.input.enqueue(make_aten_pointer_event(pixel.x, pixel.y, 0));
            return;
        }

        state_.input.enqueue(make_aten_pointer_event(pixel.x, pixel.y, aten_button_mask(state.buttons)));
    }

    void encode_keyboard(const KeyboardState&, const KeyChange& change) override
    {
        for (const KvmScancode scancode : change.released) {
            if (const auto usage = aten_keyboard_usage_from_scancode(scancode)) {
                state_.input.enqueue(make_aten_key_event(*usage, false));
            }
        }
        for (const KvmScancode scancode : change.pressed) {
            if (const auto usage = aten_keyboard_usage_from_scancode(scancode)) {
                state_.input.enqueue(make_aten_key_event(*usage, true));
            }
        }
    }

private:
    AtenViewState& state_;
};

class AtenView : public AspeedView {
public:
    explicit AtenView(const AtenViewOptions& options)
        : AtenView(options, std::make_shared<AtenViewState>())
    {
    }

    ~AtenView() override { join_media(); }

private:
    AtenView(const AtenViewOptions& options, std::shared_ptr<AtenViewState> state)
        : AspeedView(*state, options.login.base_url.host, "aten", [state] {
              state->input.clear();
              state->power.clear();
          }, default_bmc_power_caps())  // status now available via msg 53/54/55 (mouse_status byte)
        , options_(options)
        , state_(std::move(state))
        , encoder_(*state_)
        , input_(encoder_, [] { return std::optional<FrameGeometry>{}; })
        , media_controller_(
              media_state_.media,
              [this](std::string iso_path) { start_media(std::move(iso_path)); },
              [this] { request_unmount(); })
    {
    }

    void start_network(KvmNetworkWorker& network) override
    {
        AtenViewOptions network_options = options_;
        std::shared_ptr<AtenViewState> state = state_;
        network.start([network_options, state](std::atomic_bool& stop_requested) {
            run_aten_network_session(network_options, *state, stop_requested);
        });
    }

    void on_restored() override
    {
        request_full_refresh();
    }

    KvmInputController* hosted_input_controller() override
    {
        return &input_;
    }

    std::optional<std::pair<int, int>> hosted_input_resolution() override
    {
        // Host resolution from the RFB ServerInit, available before any video frame.
        const int w = state_->host_input_width.load(std::memory_order_relaxed);
        const int h = state_->host_input_height.load(std::memory_order_relaxed);
        if (w > 0 && h > 0) {
            return std::make_pair(w, h);
        }
        return std::nullopt;
    }

    void request_full_refresh() override
    {
        g_aten_full_framebuffer_refresh_requested.store(true);
    }

    void on_reset() override
    {
        state_->input.clear();
    }

    // ATEN speaks USB-BOT CD redirection on a separate /vm websocket, so it can mount a client
    // ISO. Like MegaRAC, the controller drives a dedicated media thread (start/stop below), kept
    // off the video KvmNetworkWorker so a media stop never disturbs the video session.
    VirtualMediaController* virtual_media_controller() override { return &media_controller_; }

    void start_media(std::string iso_path)
    {
        join_media();  // finish + join any prior session before starting a new mount
        media_stop_.store(false);
        AtenViewOptions options = options_;
        media_thread_ = std::thread([this, options, iso_path]() mutable {
            run_aten_media_session(options, iso_path, media_state_, media_stop_);
        });
    }

    // Eject: signal the media session to stop without blocking the GUI thread. The session
    // publishes Idle (reflected by the title-bar control) before its teardown logout, so the
    // button feels instant; the finishing thread is joined lazily by the next start / destructor.
    void request_unmount()
    {
        if (media_thread_.joinable()) {
            media_stop_.store(true);
            if (std::function<void()> force_close = media_state_.force_close_snapshot()) {
                force_close();  // graceful plug-out + close, breaking the media session's io.run()
            }
        }
    }

    void join_media()
    {
        request_unmount();
        if (media_thread_.joinable()) {
            media_thread_.join();
        }
    }

    AtenViewOptions options_;
    std::shared_ptr<AtenViewState> state_;
    AtenInputEncoder encoder_;
    KvmInputController input_;

    // Virtual media runs on its own thread (NOT the video KvmNetworkWorker): own session state
    // (channel + force-close), own stop flag. media_state_ must precede media_controller_, which
    // references its channel. media_thread_ is joined in join_media()/the destructor.
    MediaSessionState media_state_;
    std::atomic_bool media_stop_{false};
    ViewVirtualMediaController media_controller_;
    std::thread media_thread_;
};

} // namespace

std::unique_ptr<KvmViewBase> make_aten_view(const AtenViewOptions& options)
{
    return std::make_unique<AtenView>(options);
}

void run_aten_view(const AtenViewOptions& options)
{
    try {
        run_viewer({options.login.base_url.host, options.login.host_id}, [options](ViewerHost& host) {
            host.attach_view(make_aten_view(options));
        });
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "aten view ui thread");
        throw;
    }
}

} // namespace hitsc
