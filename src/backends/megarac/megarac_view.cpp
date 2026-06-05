#include "megarac_view.hpp"

#include "backends/aspeed/aspeed_view.hpp"
#include "diagnostics.hpp"
#include "gui/viewer/qt_viewer_host.hpp"
#include "hardware_cursor.hpp"
#include "megarac_hid.hpp"
#include "megarac_protocol.hpp"
#include "megarac_view_session.hpp"
#include "view_input.hpp"

#include <QImage>
#include <QRect>

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

class MegaracView : public AspeedView {
public:
    explicit MegaracView(const MegaracViewOptions& options)
        : MegaracView(options, std::make_shared<MegaracViewSessionState>())
    {
    }

private:
    MegaracView(const MegaracViewOptions& options, std::shared_ptr<MegaracViewSessionState> state)
        : AspeedView(*state, options.login.base_url.host, "megarac", [state] {
              state->input.clear();
              state->power.clear();
          }, default_bmc_power_caps())
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

    void on_restored() override
    {
        state_->frames.clear();
        request_full_refresh();
    }

    KvmInputController* hosted_input_controller() override
    {
        return &input_;
    }

    std::optional<std::pair<int, int>> hosted_input_resolution() override
    {
        // MegaRAC reports resolution only inside the video stream, so before any frame
        // we assume a default purely so pointer input flows and can wake a display-
        // asleep host. Real frame dims override this the instant video arrives.
        return std::make_pair(800, 600);
    }

    void request_full_refresh() override
    {
        state_->input.enqueue(
            MegaracInputWork{kCmdGetFullScreen, make_simple_packet(kCmdGetFullScreen, 1)});
    }

    void on_frame_presented() override
    {
        state_->video_feedback_presented_frames.fetch_add(1, std::memory_order_relaxed);
    }

    void on_reset() override
    {
        state_->input.clear();
    }

    MegaracViewOptions options_;
    std::shared_ptr<MegaracViewSessionState> state_;
    MegaracInputEncoder encoder_;
    KvmInputController input_;
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
