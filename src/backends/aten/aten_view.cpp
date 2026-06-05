#include "aten_view.hpp"

#include "backends/aspeed/aspeed_decoder.hpp"
#include "backends/aspeed/aspeed_presenter.hpp"
#include "aten_network.hpp"
#include "aten_protocol.hpp"
#include "diagnostics.hpp"
#include "gui/viewer/qt_viewer_host.hpp"
#include "hardware_cursor.hpp"
#include "view_input.hpp"

#include <QImage>
#include <QPainter>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
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
        : KvmViewBase(*state, options.login.base_url.host, "aten", [state] {
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
        hosted_clean_rgba_.clear();
        hosted_clean_width_ = 0;
        hosted_clean_height_ = 0;
        hosted_cursor_ = HardwareCursor{};
        has_hosted_cursor_ = false;
        hosted_last_sequence_ = 0;
        hosted_cursor_sequence_ = 0;
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
        bool recomposite = false;

        // BMC hardware-cursor packets arrive faster than video frames; cache the
        // latest cursor so the sprite tracks at tick rate, not frame rate.
        if (const std::shared_ptr<const HardwareCursor> cursor =
                state_->cursors.latest(hosted_cursor_sequence_)) {
            hosted_cursor_ = *cursor;
            hosted_cursor_sequence_ = cursor->sequence;
            has_hosted_cursor_ = true;
            recomposite = true;
        }

        // ASPEED is a differential stream (see FrameQueue): decode EVERY queued
        // frame in arrival order. Skipping any (as the old latest-wins mailbox
        // did) corrupts the SKIP/PASS2 delta chain until the next full refresh.
        const std::vector<std::shared_ptr<const AtenCompressedFrame>> frames =
            state_->frames.drain();
        for (const std::shared_ptr<const AtenCompressedFrame>& frame : frames) {
            hosted_last_sequence_ = frame->sequence;
            if (decode_hosted_frame(*frame)) {
                frame_presented(frame->width, frame->height);
                recomposite = true;
            }
        }
        if (state_->frames.overflowed()) {
            // Fell too far behind to preserve the delta chain; request a full
            // framebuffer refresh to resync instead of accumulating garbage.
            g_aten_full_framebuffer_refresh_requested.store(true);
        }

        bool updated = false;
        if (recomposite && !hosted_clean_rgba_.empty()) {
            if (QImage composed = compose_hosted_frame(); !composed.isNull()) {
                hosted_frame_ = std::move(composed);
                updated = true;
            }
        }
        // Only hand the surface a frame when we actually produced a NEW one this
        // tick. Returning the cached image every tick made the host re-push it,
        // which re-ran the format-convert + full GPU upload of an unchanged frame
        // ~60x/s. nullopt => the surface keeps its texture and re-draws it for free.
        if (!updated) {
            return std::nullopt;
        }
        return hosted_frame_;
    }

    std::optional<std::pair<int, int>> latest_frame_size() override
    {
        if (hosted_clean_width_ > 0 && hosted_clean_height_ > 0) {
            return std::make_pair(hosted_clean_width_, hosted_clean_height_);
        }
        if (!hosted_frame_.isNull()) {
            return std::make_pair(hosted_frame_.width(), hosted_frame_.height());
        }
        return std::nullopt;
    }

    // Delta-decode the ASPEED frame IN PLACE into hosted_clean_rgba_ (the seed for
    // the next delta): SKIP blocks keep the previous bytes, changed blocks overwrite;
    // the buffer is seeded white on the first frame / a resolution change. The cursor
    // is composited later in compose_hosted_frame so the seed never carries the
    // sprite. Returns false on bad dimensions or decode failure. The decoder is stateless.
    bool decode_hosted_frame(const AtenCompressedFrame& frame)
    {
        if (frame.width <= 0 || frame.height <= 0) {
            return false;
        }
        const std::size_t size = aspeed_frame_rgba_size(frame.width, frame.height);
        const bool reuse_previous = hosted_clean_width_ == frame.width
            && hosted_clean_height_ == frame.height
            && hosted_clean_rgba_.size() == size;
        if (!reuse_previous) {
            // First frame / resolution change: seed the delta buffer white so SKIP
            // blocks have something to keep.
            hosted_clean_rgba_.assign(size, 0xff);
        }
        // Decode IN PLACE into the persistent delta buffer (SKIP blocks leave their
        // bytes, changed blocks overwrite) -- avoids decode_rgba's wasted full-buffer
        // 0xff fill + full copy of the previous frame (~2 full-frame writes/frame).
        // decode_rgba_into validates before writing, so a bad frame throws without
        // half-updating the buffer.
        try {
            hosted_decoder_.decode_rgba_into(frame.decode_options, frame.compressed, hosted_clean_rgba_);
        } catch (...) {
            return false;
        }
        hosted_clean_width_ = frame.width;
        hosted_clean_height_ = frame.height;
        return true;
    }

    // Copy the clean frame into a fresh RGBA8888 QImage and blend the BMC cursor
    // sprite on top (SourceOver via the sprite's straight alpha; the
    // destination stays opaque). We must not paint into hosted_clean_rgba_ — it is
    // the delta seed for the next frame. The sprite goes at (cursor.x, cursor.y):
    // make_cursor_image already bakes the pattern x/y_offset into the sampled
    // sprite, so its top-left maps to (cursor.x, cursor.y).
    QImage compose_hosted_frame() const
    {
        const std::size_t size = aspeed_frame_rgba_size(hosted_clean_width_, hosted_clean_height_);
        QImage image(hosted_clean_width_, hosted_clean_height_, QImage::Format_RGBA8888);
        if (hosted_clean_rgba_.size() != size
            || static_cast<std::size_t>(image.bytesPerLine()) * static_cast<std::size_t>(hosted_clean_height_) != size) {
            return {};
        }
        std::memcpy(image.bits(), hosted_clean_rgba_.data(), size);

        if (has_hosted_cursor_ && hosted_cursor_.visible) {
            const CursorImage cursor_image = make_cursor_image(
                hosted_cursor_, hosted_clean_rgba_, hosted_clean_width_, hosted_clean_height_);
            if (!cursor_image.rgba.empty() && cursor_image.width > 0 && cursor_image.height > 0) {
                const QImage sprite(
                    reinterpret_cast<const uchar*>(cursor_image.rgba.data()),
                    cursor_image.width,
                    cursor_image.height,
                    QImage::Format_RGBA8888);
                QPainter painter(&image);
                painter.drawImage(QPoint(hosted_cursor_.x, hosted_cursor_.y), sprite);
            }
        }
        return image;
    }

    AtenViewOptions options_;
    std::shared_ptr<AtenViewState> state_;
    AtenInputEncoder encoder_;
    KvmInputController input_;
    QImage hosted_frame_;                          // displayed: clean frame + BMC cursor
    std::vector<std::uint8_t> hosted_clean_rgba_;  // clean decoded RGBA; delta seed (no cursor)
    int hosted_clean_width_ = 0;
    int hosted_clean_height_ = 0;
    HardwareCursor hosted_cursor_;
    bool has_hosted_cursor_ = false;
    std::uint64_t hosted_last_sequence_ = 0;
    std::uint64_t hosted_cursor_sequence_ = 0;
    AspeedDecoder hosted_decoder_;
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
