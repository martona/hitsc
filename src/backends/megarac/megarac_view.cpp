#include "megarac_view.hpp"

#include "backends/aspeed/aspeed_decoder.hpp"
#include "backends/aspeed/aspeed_presenter.hpp"
#include "diagnostics.hpp"
#include "gui/viewer/qt_viewer_host.hpp"
#include "hardware_cursor.hpp"
#include "megarac_hid.hpp"
#include "megarac_protocol.hpp"
#include "megarac_view_session.hpp"
#include "view_input.hpp"

#include <QImage>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

// Uncomment to log how many ASPEED frames are drained (decoded) per view tick.
// A count > 1 means frames arrived faster than the ~16 ms tick — exactly the
// intermediate differential frames the old latest-wins mailbox used to drop.
// #define HITSC_MEGARAC_FRAME_DEBUG 1

namespace hitsc {
namespace {

constexpr std::uint16_t kCmdSendHidPacket = command_value(MegaracCommand::SendHidPacket);
constexpr std::uint16_t kCmdGetFullScreen = command_value(MegaracCommand::GetFullScreen);

#ifdef HITSC_DEBUG_HW_CURSOR
// Debug: true if the sprite carries real cursor content (any pixel-to-pixel
// variation), false if it is a uniform blob / fully transparent. Distinguishes a
// live hardware cursor from a degenerate/blank one in the title diagnostic.
bool cursor_sprite_has_variation(const QImage& sprite)
{
    if (sprite.isNull()) {
        return false;
    }
    const QImage rgba = sprite.format() == QImage::Format_RGBA8888
        ? sprite
        : sprite.convertToFormat(QImage::Format_RGBA8888);
    bool seen = false;
    std::uint32_t first = 0;
    for (int y = 0; y < rgba.height(); ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(rgba.constScanLine(y));
        for (int x = 0; x < rgba.width(); ++x) {
            if (!seen) {
                first = row[x];
                seen = true;
            } else if (row[x] != first) {
                return true;
            }
        }
    }
    return false;
}
#endif

std::optional<std::uint8_t> keyboard_modifier_bit(KvmScancode scancode)
{
    switch (scancode) {
    case KvmScancode::LCTRL:
        return kMegaracKeyboardLeftCtrl;
    case KvmScancode::LSHIFT:
        return kMegaracKeyboardLeftShift;
    case KvmScancode::LALT:
        return kMegaracKeyboardLeftAlt;
    case KvmScancode::LGUI:
        return kMegaracKeyboardLeftGui;
    case KvmScancode::RCTRL:
        return kMegaracKeyboardRightCtrl;
    case KvmScancode::RSHIFT:
        return kMegaracKeyboardRightShift;
    case KvmScancode::RALT:
        return kMegaracKeyboardRightAlt;
    case KvmScancode::RGUI:
        return kMegaracKeyboardRightGui;
    default:
        return std::nullopt;
    }
}

std::optional<std::uint8_t> keyboard_usage_from_scancode(KvmScancode scancode)
{
    const auto usage = static_cast<int>(scancode);
    if ((usage >= static_cast<int>(KvmScancode::A) && usage <= static_cast<int>(KvmScancode::APPLICATION)) ||
        (usage >= static_cast<int>(KvmScancode::KP_EQUALS) && usage <= static_cast<int>(KvmScancode::F24))) {
        return static_cast<std::uint8_t>(usage);
    }

    return std::nullopt;
}

std::uint8_t megarac_button_mask(std::uint32_t buttons)
{
    std::uint8_t mask = 0;
    if (buttons & (1u << static_cast<unsigned>(KvmMouseButton::LEFT))) {
        mask |= kMegaracMouseLeftButton;
    }
    if (buttons & (1u << static_cast<unsigned>(KvmMouseButton::RIGHT))) {
        mask |= kMegaracMouseRightButton;
    }
    if (buttons & (1u << static_cast<unsigned>(KvmMouseButton::MIDDLE))) {
        mask |= kMegaracMouseMiddleButton;
    }
    return mask;
}

class MegaracInputEncoder : public KvmInputEncoder {
public:
    explicit MegaracInputEncoder(MegaracViewSessionState& state)
        : state_(state)
    {
    }

    bool accepts_button(KvmMouseButton button) const override
    {
        return button == KvmMouseButton::LEFT || button == KvmMouseButton::MIDDLE || button == KvmMouseButton::RIGHT;
    }

    bool accepts_key(KvmScancode scancode) const override
    {
        return keyboard_modifier_bit(scancode).has_value()
            || keyboard_usage_from_scancode(scancode).has_value();
    }

    void encode_pointer(const PointerState& state, const PointerChange& change) override
    {
        const FramePixel pixel = to_frame_pixel(state.position, state.frame_width, state.frame_height);
        const std::uint8_t buttons = megarac_button_mask(state.buttons);
        const int wheel = change.kind == PointerChange::Kind::Wheel ? static_cast<int>(change.wheel_y) : 0;

        const int mouse_mode = megarac_view_mouse_mode_snapshot(state_);
        std::vector<std::uint8_t> packet;
        if (mouse_mode == kMegaracRelativeMouseMode || mouse_mode == kMegaracOtherMouseMode) {
            const int dx = last_position_ ? pixel.x - last_position_->x : 0;
            const int dy = last_position_ ? pixel.y - last_position_->y : 0;
            packet = make_megarac_relative_mouse_packet(
                MegaracRelativeMouseReport{buttons, dx, dy, wheel},
                mouse_sequence_++);
        } else {
            packet = make_megarac_absolute_mouse_packet(
                MegaracAbsoluteMouseReport{buttons, pixel.x, pixel.y, state.frame_width, state.frame_height, wheel},
                mouse_sequence_++);
        }

        last_position_ = pixel;
        state_.input.enqueue(MegaracInputWork{kCmdSendHidPacket, std::move(packet)});
    }

    void encode_keyboard(const KeyboardState& state, const KeyChange&) override
    {
        std::uint8_t modifiers = 0;
        MegaracKeyboardKeySlots keys{};
        std::size_t slot = 0;
        for (std::size_t scancode = 0; scancode < state.down.size(); ++scancode) {
            if (!state.down[scancode]) {
                continue;
            }
            const auto code = static_cast<KvmScancode>(scancode);
            if (const auto modifier = keyboard_modifier_bit(code)) {
                modifiers |= *modifier;
                continue;
            }
            if (const auto usage = keyboard_usage_from_scancode(code)) {
                if (slot < keys.size()) {
                    keys[slot++] = *usage;
                }
            }
        }

        state_.input.enqueue(MegaracInputWork{
            kCmdSendHidPacket,
            make_megarac_keyboard_packet(MegaracKeyboardReport{modifiers, keys}, keyboard_sequence_++)});
    }

private:
    MegaracViewSessionState& state_;
    std::uint32_t mouse_sequence_ = 0;
    std::uint32_t keyboard_sequence_ = 0;
    std::optional<FramePixel> last_position_;
};

class MegaracView : public KvmViewBase {
public:
    explicit MegaracView(const MegaracViewOptions& options)
        : MegaracView(options, std::make_shared<MegaracViewSessionState>())
    {
    }

private:
    MegaracView(const MegaracViewOptions& options, std::shared_ptr<MegaracViewSessionState> state)
        : KvmViewBase(*state, options.login.base_url.host, "megarac", [state] {
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
        MegaracViewOptions network_options = options_;
        std::shared_ptr<MegaracViewSessionState> state = state_;
        network.start([network_options, state](std::atomic_bool& stop_requested) {
            run_megarac_view_session(network_options, *state, stop_requested);
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
        state_->frames.clear();
        state_->input.enqueue(MegaracInputWork{kCmdGetFullScreen, make_simple_packet(kCmdGetFullScreen, 1)});
    }

    void on_focus_lost() override
    {
        input_.release_all_keys();
    }

    KvmInputController* hosted_input_controller() override
    {
        return &input_;
    }

    std::optional<SoftwareFrame> latest_frame() override
    {
#ifdef HITSC_DEBUG_HW_CURSOR
        const std::uint64_t debug_cursor_seq_before = hosted_cursor_sequence_;
#endif
        // BMC hardware-cursor packets arrive faster than video frames; cache the
        // latest cursor so the sprite tracks at tick rate, not frame rate.
        bool cursor_changed = false;
        if (const std::shared_ptr<const HardwareCursor> cursor =
                state_->cursors.latest(hosted_cursor_sequence_)) {
            hosted_cursor_ = *cursor;
            hosted_cursor_sequence_ = cursor->sequence;
            has_hosted_cursor_ = true;
            cursor_changed = true;
        }

        // ASPEED is a differential stream (see FrameQueue): decode EVERY queued
        // frame in arrival order. Skipping any (as the old latest-wins mailbox
        // did) corrupts the SKIP/PASS2 delta chain until the next keyframe — the
        // root cause of the fast-host garbling.
        bool base_changed = false;
        const std::vector<std::shared_ptr<const MegaracCompressedFrame>> frames =
            state_->frames.drain();
#ifdef HITSC_MEGARAC_FRAME_DEBUG
        if (frames.size() > 1) {
            std::cerr << "[megarac] drained " << frames.size()
                      << " frames this tick (latest-wins would have dropped "
                      << (frames.size() - 1) << ")" << std::endl;
        }
#endif
        for (const std::shared_ptr<const MegaracCompressedFrame>& frame : frames) {
            hosted_last_sequence_ = frame->sequence;
            if (decode_hosted_frame(*frame)) {
                frame_presented(frame->width, frame->height);
                state_->video_feedback_presented_frames.fetch_add(1, std::memory_order_relaxed);
                base_changed = true;
            }
        }
        if (state_->frames.overflowed()) {
            // Fell too far behind to preserve the delta chain; request a full
            // keyframe to resync instead of accumulating garbage.
            state_->input.enqueue(
                MegaracInputWork{kCmdGetFullScreen, make_simple_packet(kCmdGetFullScreen, 1)});
        }

        SoftwareFrame out;
        if (base_changed && !hosted_clean_rgba_.empty()) {
            if (QImage base = make_base_image(); !base.isNull()) {
                hosted_frame_ = base;
                out.base = std::move(base);
            }
        }
        // Rebuild the cursor sprite when it moved OR when the video under it
        // changed: type-non-1 XOR cursors invert the pixels beneath them, so a
        // base change must re-bake the sprite even if the cursor stayed put. The
        // large base texture stays decoupled -- only the tiny sprite re-uploads.
        if ((cursor_changed || base_changed) && has_hosted_cursor_) {
            out.cursor = build_cursor_overlay();
        }

#ifdef HITSC_DEBUG_HW_CURSOR
        {
            const unsigned debug_packets =
                static_cast<unsigned>(hosted_cursor_sequence_ - debug_cursor_seq_before);
            const bool debug_valid = out.cursor && !out.cursor->sprite.isNull()
                && cursor_sprite_has_variation(out.cursor->sprite);
            state_->view_status.debug_cursor_tick(
                debug_packets, debug_valid, hosted_cursor_.width, hosted_cursor_.height,
                hosted_cursor_.x, hosted_cursor_.y, hosted_cursor_.type);
        }
#endif

        // nullopt when nothing changed this tick => the surface re-draws its
        // existing base + cursor textures for free (no convert, no upload).
        if (!out.base && !out.cursor) {
            return std::nullopt;
        }
        return out;
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

    // See AtenView::decode_hosted_frame / make_base_image. Delta-decode into
    // hosted_clean_rgba_ (the cursor-free delta seed); the BMC cursor is composited
    // later as a separate GPU overlay quad so the seed never carries the sprite.
    bool decode_hosted_frame(const MegaracCompressedFrame& frame)
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

    // Copy the cursor-free clean frame into a fresh RGBA8888 QImage for upload as
    // the base texture (no cursor baked in -- it is a separate GPU overlay quad,
    // and hosted_clean_rgba_ is the delta seed for the next frame).
    QImage make_base_image() const
    {
        const std::size_t size = aspeed_frame_rgba_size(hosted_clean_width_, hosted_clean_height_);
        QImage image(hosted_clean_width_, hosted_clean_height_, QImage::Format_RGBA8888);
        if (hosted_clean_rgba_.size() != size
            || static_cast<std::size_t>(image.bytesPerLine()) * static_cast<std::size_t>(hosted_clean_height_) != size) {
            return {};
        }
        std::memcpy(image.bits(), hosted_clean_rgba_.data(), size);
        return image;
    }

    // Build the cursor sprite for the GPU overlay. make_cursor_image bakes the
    // pattern x/y_offset into the sampled sprite (top-left maps to cursor.x/y); for
    // XOR cursors it reads hosted_clean_rgba_ to invert the background, so this is
    // rebuilt whenever the base changes. A null sprite => hide.
    CursorOverlay build_cursor_overlay() const
    {
        CursorOverlay overlay;
        overlay.x = hosted_cursor_.x;
        overlay.y = hosted_cursor_.y;
        if (!hosted_cursor_.visible) {
            return overlay;  // empty sprite => hide
        }
        const CursorImage cursor_image = make_cursor_image(
            hosted_cursor_, hosted_clean_rgba_, hosted_clean_width_, hosted_clean_height_);
        if (cursor_image.rgba.empty() || cursor_image.width <= 0 || cursor_image.height <= 0) {
            return overlay;  // nothing to show => hide
        }
        const QImage wrapped(
            reinterpret_cast<const uchar*>(cursor_image.rgba.data()),
            cursor_image.width,
            cursor_image.height,
            QImage::Format_RGBA8888);
        overlay.sprite = wrapped.copy();  // own the bytes (cursor_image is a local)
        return overlay;
    }

    MegaracViewOptions options_;
    std::shared_ptr<MegaracViewSessionState> state_;
    MegaracInputEncoder encoder_;
    KvmInputController input_;
    QImage hosted_frame_;                          // last base frame (cursor is a separate GPU overlay)
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

std::unique_ptr<KvmViewBase> make_megarac_view(const MegaracViewOptions& options)
{
    return std::make_unique<MegaracView>(options);
}

void run_megarac_view(const MegaracViewOptions& options)
{
    try {
        run_viewer({options.login.base_url.host, options.login.host_id}, [options](ViewerHost& host) {
            host.attach_view(make_megarac_view(options));
        });
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "megarac view ui thread");
        throw;
    }
}

} // namespace hitsc
