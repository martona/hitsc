#include "megarac_view.hpp"

#include "backends/aspeed/aspeed_view_renderer.hpp"
#include "diagnostics.hpp"
#include "megarac_hid.hpp"
#include "megarac_protocol.hpp"
#include "megarac_view_session.hpp"
#include "view_input.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
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

std::optional<std::uint8_t> keyboard_modifier_bit(SDL_Scancode scancode)
{
    switch (scancode) {
    case SDL_SCANCODE_LCTRL:
        return kMegaracKeyboardLeftCtrl;
    case SDL_SCANCODE_LSHIFT:
        return kMegaracKeyboardLeftShift;
    case SDL_SCANCODE_LALT:
        return kMegaracKeyboardLeftAlt;
    case SDL_SCANCODE_LGUI:
        return kMegaracKeyboardLeftGui;
    case SDL_SCANCODE_RCTRL:
        return kMegaracKeyboardRightCtrl;
    case SDL_SCANCODE_RSHIFT:
        return kMegaracKeyboardRightShift;
    case SDL_SCANCODE_RALT:
        return kMegaracKeyboardRightAlt;
    case SDL_SCANCODE_RGUI:
        return kMegaracKeyboardRightGui;
    default:
        return std::nullopt;
    }
}

std::optional<std::uint8_t> keyboard_usage_from_sdl_scancode(SDL_Scancode scancode)
{
    const auto usage = static_cast<int>(scancode);
    if ((usage >= SDL_SCANCODE_A && usage <= SDL_SCANCODE_APPLICATION) ||
        (usage >= SDL_SCANCODE_KP_EQUALS && usage <= SDL_SCANCODE_F24)) {
        return static_cast<std::uint8_t>(usage);
    }

    return std::nullopt;
}

std::uint8_t megarac_button_mask(std::uint32_t buttons)
{
    std::uint8_t mask = 0;
    if (buttons & (1u << SDL_BUTTON_LEFT)) {
        mask |= kMegaracMouseLeftButton;
    }
    if (buttons & (1u << SDL_BUTTON_RIGHT)) {
        mask |= kMegaracMouseRightButton;
    }
    if (buttons & (1u << SDL_BUTTON_MIDDLE)) {
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

    bool accepts_button(std::uint8_t button) const override
    {
        return button == SDL_BUTTON_LEFT || button == SDL_BUTTON_MIDDLE || button == SDL_BUTTON_RIGHT;
    }

    bool accepts_key(SDL_Scancode scancode) const override
    {
        return keyboard_modifier_bit(scancode).has_value()
            || keyboard_usage_from_sdl_scancode(scancode).has_value();
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
            const auto code = static_cast<SDL_Scancode>(scancode);
            if (const auto modifier = keyboard_modifier_bit(code)) {
                modifiers |= *modifier;
                continue;
            }
            if (const auto usage = keyboard_usage_from_sdl_scancode(code)) {
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
        , input_(encoder_, [this] { return frame_geometry(); })
    {
    }

    std::optional<FrameGeometry> frame_geometry() const
    {
        const AspeedPresentationSlot* active = aspeed_.active_slot();
        if (active == nullptr) {
            return std::nullopt;
        }
        return FrameGeometry{
            active->width,
            active->height,
            current_target_rect(active->width, active->height)};
    }

    void start_network(KvmNetworkWorker& network) override
    {
        MegaracViewOptions network_options = options_;
        std::shared_ptr<MegaracViewSessionState> state = state_;
        network.start([network_options, state](std::atomic_bool& stop_requested) {
            run_megarac_view_session(network_options, *state, stop_requested);
        });
    }

    void before_sdl_cleanup() override
    {
        input_.reset();
        aspeed_.destroy();
    }

    void reset_for_reconnect() override
    {
        state_->frames.clear();
        state_->cursors.clear();
        state_->input.clear();
        aspeed_.destroy();
        aspeed_.reset_sequences();
        input_.reset();
    }

    void on_minimized() override
    {
        state_->frames.clear();
        aspeed_.destroy();
        aspeed_.reset_sequences();
    }

    void on_restored() override
    {
        state_->frames.clear();
        aspeed_.reset_sequences();
        state_->input.enqueue(MegaracInputWork{kCmdGetFullScreen, make_simple_packet(kCmdGetFullScreen, 1)});
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
        bool presented_new_frame = false;
        aspeed_.update(
            renderer(),
            state_->frames,
            state_->cursors,
            "MegaRAC",
            options_.login.vverbose,
            render_needed,
            presented_new_frame);

        if (!render_needed) {
            return;
        }

        clear_background();
        if (const AspeedPresentationSlot* active = aspeed_.active_slot();
            active != nullptr && active->texture != nullptr) {
            aspeed_.render(renderer(), current_target_rect(active->width, active->height));
        }
        present();

        if (presented_new_frame) {
            if (const AspeedPresentationSlot* active = aspeed_.active_slot()) {
                frame_presented(active->width, active->height);
                state_->video_feedback_presented_frames.fetch_add(1, std::memory_order_relaxed);
            }
        }
        first_render = false;
    }

    MegaracViewOptions options_;
    std::shared_ptr<MegaracViewSessionState> state_;
    AspeedViewRenderer aspeed_;
    MegaracInputEncoder encoder_;
    KvmInputController input_;
};

} // namespace

void run_megarac_view(const MegaracViewOptions& options, const ViewWindow* handoff)
{
    try {
        MegaracView view(options);
        if (handoff != nullptr) {
            view.adopt_sdl(*handoff);
        }
        view.run();
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "megarac view ui thread");
        throw;
    }
}

} // namespace hitsc
