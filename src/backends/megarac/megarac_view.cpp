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
#include <QPainter>

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

namespace hitsc {
namespace {

constexpr std::uint16_t kCmdSendHidPacket = command_value(MegaracCommand::SendHidPacket);
constexpr std::uint16_t kCmdGetFullScreen = command_value(MegaracCommand::GetFullScreen);

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
        : KvmViewBase(*state, options.login.base_url.host, options.login.host_id, "megarac", [state] {
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

        const std::shared_ptr<const MegaracCompressedFrame> frame =
            state_->frames.latest(hosted_last_sequence_);
        if (frame) {
            hosted_last_sequence_ = frame->sequence;
            if (decode_hosted_frame(*frame)) {
                frame_presented(frame->width, frame->height);
                state_->video_feedback_presented_frames.fetch_add(1, std::memory_order_relaxed);
                recomposite = true;
            }
        }

        if (recomposite && !hosted_clean_rgba_.empty()) {
            if (QImage composed = compose_hosted_frame(); !composed.isNull()) {
                hosted_frame_ = std::move(composed);
            }
        }
        if (hosted_frame_.isNull()) {
            return std::nullopt;
        }
        return hosted_frame_;
    }

    std::optional<std::pair<int, int>> latest_frame_size() override
    {
        if (const std::shared_ptr<const MegaracCompressedFrame> frame = state_->frames.latest(0)) {
            return std::make_pair(frame->width, frame->height);
        }
        if (!hosted_frame_.isNull()) {
            return std::make_pair(hosted_frame_.width(), hosted_frame_.height());
        }
        return std::nullopt;
    }

    // See AtenView::decode_hosted_frame / compose_hosted_frame. Delta-decode into
    // hosted_clean_rgba_ (the cursor-free delta seed); the BMC cursor is blended
    // on top later so the seed never carries the sprite.
    bool decode_hosted_frame(const MegaracCompressedFrame& frame)
    {
        if (frame.width <= 0 || frame.height <= 0) {
            return false;
        }
        const std::size_t size = aspeed_frame_rgba_size(frame.width, frame.height);
        const bool reuse_previous = hosted_clean_width_ == frame.width
            && hosted_clean_height_ == frame.height
            && hosted_clean_rgba_.size() == size;
        try {
            hosted_clean_rgba_ = hosted_decoder_.decode_rgba(
                frame.decode_options,
                frame.compressed,
                reuse_previous ? &hosted_clean_rgba_ : nullptr);
        } catch (...) {
            return false;
        }
        hosted_clean_width_ = frame.width;
        hosted_clean_height_ = frame.height;
        return true;
    }

    QImage compose_hosted_frame() const
    {
        const std::size_t size = aspeed_frame_rgba_size(hosted_clean_width_, hosted_clean_height_);
        QImage image(hosted_clean_width_, hosted_clean_height_, QImage::Format_RGBX8888);
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

    MegaracViewOptions options_;
    std::shared_ptr<MegaracViewSessionState> state_;
    MegaracInputEncoder encoder_;
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

void run_megarac_view(const MegaracViewOptions& options)
{
    try {
        MegaracView view(options);
        run_qt_viewer(view, options.login.base_url.host, options.login.host_id);
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "megarac view ui thread");
        throw;
    }
}

} // namespace hitsc
