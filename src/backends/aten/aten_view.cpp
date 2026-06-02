#include "aten_view.hpp"

#include "backends/aspeed/aspeed_view_renderer.hpp"
#include "aten_network.hpp"
#include "aten_protocol.hpp"
#include "diagnostics.hpp"
#include "view_input.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace hitsc {

extern std::atomic_bool g_aten_full_framebuffer_refresh_requested;

namespace {

std::optional<std::uint32_t> aten_keyboard_usage_from_sdl_scancode(SDL_Scancode scancode)
{
    const auto usage = static_cast<int>(scancode);
    if ((usage >= SDL_SCANCODE_A && usage <= SDL_SCANCODE_APPLICATION) ||
        (usage >= SDL_SCANCODE_KP_EQUALS && usage <= SDL_SCANCODE_RGUI)) {
        return static_cast<std::uint32_t>(usage);
    }

    return std::nullopt;
}

std::uint8_t aten_button_mask(std::uint32_t buttons)
{
    std::uint8_t mask = 0;
    if (buttons & (1u << SDL_BUTTON_LEFT)) {
        mask |= 1;
    }
    if (buttons & (1u << SDL_BUTTON_MIDDLE)) {
        mask |= 2;
    }
    if (buttons & (1u << SDL_BUTTON_RIGHT)) {
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

    bool accepts_button(std::uint8_t button) const override
    {
        return button == SDL_BUTTON_LEFT || button == SDL_BUTTON_MIDDLE || button == SDL_BUTTON_RIGHT;
    }

    bool accepts_key(SDL_Scancode scancode) const override
    {
        return aten_keyboard_usage_from_sdl_scancode(scancode).has_value();
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
        for (const SDL_Scancode scancode : change.released) {
            if (const auto usage = aten_keyboard_usage_from_sdl_scancode(scancode)) {
                state_.input.enqueue(make_aten_key_event(*usage, false));
            }
        }
        for (const SDL_Scancode scancode : change.pressed) {
            if (const auto usage = aten_keyboard_usage_from_sdl_scancode(scancode)) {
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
        AtenViewOptions network_options = options_;
        std::shared_ptr<AtenViewState> state = state_;
        network.start([network_options, state](std::atomic_bool& stop_requested) {
            run_aten_network_session(network_options, *state, stop_requested);
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
        g_aten_full_framebuffer_refresh_requested.store(true);
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
            "ATEN",
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
            }
        }
        first_render = false;
    }

    AtenViewOptions options_;
    std::shared_ptr<AtenViewState> state_;
    AspeedViewRenderer aspeed_;
    AtenInputEncoder encoder_;
    KvmInputController input_;
};

} // namespace

void run_aten_view(const AtenViewOptions& options)
{
    try {
        AtenView view(options);
        view.run();
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "aten view ui thread");
        throw;
    }
}

} // namespace hitsc
