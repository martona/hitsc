#include "aten_view.hpp"

#include "backends/aspeed/aspeed_decoder.hpp"
#include "backends/aspeed/aspeed_presenter.hpp"
#include "aten_network.hpp"
#include "aten_protocol.hpp"
#include "diagnostics.hpp"
#include "gui/viewer/qt_viewer_host.hpp"
#include "view_input.hpp"

#include <QImage>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
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

class AtenView : public KvmViewBase {
public:
    explicit AtenView(const AtenViewOptions& options)
        : AtenView(options, std::make_shared<AtenViewState>())
    {
    }

private:
    AtenView(const AtenViewOptions& options, std::shared_ptr<AtenViewState> state)
        : KvmViewBase(*state, options.login.base_url.host, options.login.host_id, "aten", [state] {
              state->input.clear();
          })
        , options_(options)
        , state_(std::move(state))
        , encoder_(*state_)
        , input_(encoder_, [] { return std::optional<FrameGeometry>{}; })
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

    void reset_for_reconnect() override
    {
        state_->frames.clear();
        state_->cursors.clear();
        state_->input.clear();
        input_.reset();
        hosted_frame_ = QImage();
        hosted_last_sequence_ = 0;
    }

    void on_minimized() override
    {
        state_->frames.clear();
    }

    void on_restored() override
    {
        g_aten_full_framebuffer_refresh_requested.store(true);
    }

    void on_focus_lost() override
    {
        input_.release_all_keys();
    }

    KvmInputController* hosted_input_controller() override
    {
        return &input_;
    }

    std::optional<QImage> latest_frame_image() override
    {
        const std::shared_ptr<const AtenCompressedFrame> frame =
            state_->frames.latest(hosted_last_sequence_);
        if (frame) {
            hosted_last_sequence_ = frame->sequence;
            if (std::optional<QImage> image = decode_hosted_frame(*frame)) {
                hosted_frame_ = std::move(*image);
                frame_presented(frame->width, frame->height);
            }
        }
        if (hosted_frame_.isNull()) {
            return std::nullopt;
        }
        return hosted_frame_;
    }

    std::optional<std::pair<int, int>> latest_frame_size() override
    {
        if (const std::shared_ptr<const AtenCompressedFrame> frame = state_->frames.latest(0)) {
            return std::make_pair(frame->width, frame->height);
        }
        if (!hosted_frame_.isNull()) {
            return std::make_pair(hosted_frame_.width(), hosted_frame_.height());
        }
        return std::nullopt;
    }

    // Decode an ASPEED compressed frame to an opaque RGBA QImage for the Qt
    // surface, replicating the presenter's delta model: seed from the previous
    // frame (or white) then decode the update on top. RGBX8888 ignores the alpha
    // byte so the frame always renders opaque. The decoder is stateless, so a
    // private instance is fine. (Hardware-cursor overlay is deferred to the GPU/
    // overlay work; this shows guest video + accepts input.)
    std::optional<QImage> decode_hosted_frame(const AtenCompressedFrame& frame)
    {
        if (frame.width <= 0 || frame.height <= 0) {
            return std::nullopt;
        }
        const std::size_t size = aspeed_frame_rgba_size(frame.width, frame.height);
        QImage image(frame.width, frame.height, QImage::Format_RGBX8888);
        if (static_cast<std::size_t>(image.bytesPerLine()) * static_cast<std::size_t>(frame.height) != size) {
            return std::nullopt;
        }
        if (!hosted_frame_.isNull()
            && hosted_frame_.width() == frame.width
            && hosted_frame_.height() == frame.height
            && hosted_frame_.format() == QImage::Format_RGBX8888) {
            std::memcpy(image.bits(), hosted_frame_.constBits(), size);
        } else {
            image.fill(Qt::white);
        }
        try {
            hosted_decoder_.decode_rgba_into(
                frame.decode_options,
                frame.compressed,
                std::span<std::uint8_t>(image.bits(), size));
        } catch (...) {
            return std::nullopt;
        }
        return image;
    }

    AtenViewOptions options_;
    std::shared_ptr<AtenViewState> state_;
    AtenInputEncoder encoder_;
    KvmInputController input_;
    QImage hosted_frame_;
    std::uint64_t hosted_last_sequence_ = 0;
    AspeedDecoder hosted_decoder_;
};

} // namespace

void run_aten_view(const AtenViewOptions& options)
{
    try {
        AtenView view(options);
        run_qt_viewer(view, options.login.base_url.host, options.login.host_id);
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "aten view ui thread");
        throw;
    }
}

} // namespace hitsc
