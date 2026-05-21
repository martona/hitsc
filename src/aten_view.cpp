#include "aten_view.hpp"

#include "aspeed_view_renderer.hpp"
#include "aten_network.hpp"
#include "aten_protocol.hpp"
#include "diagnostics.hpp"
#include "log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>

namespace hitsc {

extern std::atomic_bool g_aten_full_framebuffer_refresh_requested;

namespace {

constexpr std::uint64_t kMouseMotionIntervalMilliseconds = 8;
using AtenKeyDownState = std::array<bool, 256>;

struct AtenRemoteMousePosition {
    int x = 0;
    int y = 0;
};

std::optional<AtenRemoteMousePosition> remote_mouse_position(
    float window_x,
    float window_y,
    const SDL_FRect& target,
    int frame_width,
    int frame_height,
    bool clamp_to_target)
{
    if (frame_width <= 0 || frame_height <= 0 || target.w <= 0.0f || target.h <= 0.0f) {
        return std::nullopt;
    }

    const bool inside =
        window_x >= target.x
        && window_y >= target.y
        && window_x <= target.x + target.w
        && window_y <= target.y + target.h;
    if (!inside && !clamp_to_target) {
        return std::nullopt;
    }

    const float clamped_x = std::clamp(window_x, target.x, target.x + target.w);
    const float clamped_y = std::clamp(window_y, target.y, target.y + target.h);
    const double relative_x =
        (static_cast<double>(clamped_x) - static_cast<double>(target.x)) / static_cast<double>(target.w);
    const double relative_y =
        (static_cast<double>(clamped_y) - static_cast<double>(target.y)) / static_cast<double>(target.h);
    return AtenRemoteMousePosition{
        std::clamp(static_cast<int>(relative_x * frame_width + 0.5), 0, frame_width),
        std::clamp(static_cast<int>(relative_y * frame_height + 0.5), 0, frame_height)};
}

std::uint8_t button_mask_for_sdl_button(std::uint8_t button)
{
    switch (button) {
    case SDL_BUTTON_LEFT:
        return 1;
    case SDL_BUTTON_MIDDLE:
        return 2;
    case SDL_BUTTON_RIGHT:
        return 4;
    default:
        return 0;
    }
}

std::optional<std::uint32_t> aten_keyboard_usage_from_sdl_scancode(SDL_Scancode scancode)
{
    const auto usage = static_cast<int>(scancode);
    if ((usage >= SDL_SCANCODE_A && usage <= SDL_SCANCODE_APPLICATION) ||
        (usage >= SDL_SCANCODE_KP_EQUALS && usage <= SDL_SCANCODE_RGUI)) {
        return static_cast<std::uint32_t>(usage);
    }

    return std::nullopt;
}

void queue_aten_key_event(AtenViewState& state, std::uint32_t usage, bool down)
{
    state.input.enqueue(make_aten_key_event(usage, down));
}

void queue_aten_pointer_event(AtenViewState& state, int x, int y, std::uint8_t mask)
{
    state.input.enqueue(make_aten_pointer_event(x, y, mask));
}

void release_all_aten_keys(AtenViewState& state, AtenKeyDownState& key_down)
{
    for (std::size_t usage = 0; usage < key_down.size(); ++usage) {
        if (!key_down[usage]) {
            continue;
        }
        key_down[usage] = false;
        queue_aten_key_event(state, static_cast<std::uint32_t>(usage), false);
    }
}

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

    void before_sdl_cleanup() override
    {
        aspeed_.destroy();
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
        release_all_aten_keys(*state_, key_down_);
    }

    void handle_event(const SDL_Event& event, bool&) override
    {
        const AspeedPresentationSlot* active = aspeed_.active_slot();
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            handle_key_event(event);
        } else if (active != nullptr &&
                   (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                    event.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
            handle_mouse_button_event(event, *active);
        } else if (active != nullptr && event.type == SDL_EVENT_MOUSE_MOTION) {
            handle_mouse_motion_event(event, *active);
        } else if (active != nullptr && event.type == SDL_EVENT_MOUSE_WHEEL) {
            handle_mouse_wheel_event(event, *active);
        }
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

    void handle_key_event(const SDL_Event& event)
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat) {
            return;
        }

        const std::optional<std::uint32_t> usage =
            aten_keyboard_usage_from_sdl_scancode(event.key.scancode);
        if (!usage || *usage >= key_down_.size()) {
            if (options_.login.vverbose) {
                log_info() << "ignored ATEN key"
                           << " scancode=" << event.key.scancode
                           << " key=" << event.key.key;
            }
            return;
        }

        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (key_down_[*usage] != down) {
            key_down_[*usage] = down;
            queue_aten_key_event(*state_, *usage, down);
        }
    }

    void handle_mouse_button_event(const SDL_Event& event, const AspeedPresentationSlot& active)
    {
        const std::uint8_t mask = button_mask_for_sdl_button(event.button.button);
        if (mask == 0) {
            return;
        }

        const SDL_FRect target = current_target_rect(active.width, active.height);
        const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        const bool drag_active = mouse_buttons_ != 0;
        const std::optional<AtenRemoteMousePosition> position = remote_mouse_position(
            event.button.x,
            event.button.y,
            target,
            active.width,
            active.height,
            drag_active || !down);
        if (!position) {
            return;
        }

        if (down) {
            mouse_buttons_ |= mask;
        } else {
            mouse_buttons_ &= static_cast<std::uint8_t>(~mask);
        }
        SDL_CaptureMouse(mouse_buttons_ != 0);
        queue_aten_pointer_event(*state_, position->x, position->y, mouse_buttons_);
    }

    void handle_mouse_motion_event(const SDL_Event& event, const AspeedPresentationSlot& active)
    {
        const std::uint64_t ticks = SDL_GetTicks();
        const bool throttled =
            mouse_buttons_ == 0 &&
            ticks - last_mouse_motion_ticks_ < kMouseMotionIntervalMilliseconds;
        if (throttled) {
            return;
        }

        const SDL_FRect target = current_target_rect(active.width, active.height);
        const std::optional<AtenRemoteMousePosition> position = remote_mouse_position(
            event.motion.x,
            event.motion.y,
            target,
            active.width,
            active.height,
            mouse_buttons_ != 0);
        if (position) {
            queue_aten_pointer_event(*state_, position->x, position->y, mouse_buttons_);
            last_mouse_motion_ticks_ = ticks;
        }
    }

    void handle_mouse_wheel_event(const SDL_Event& event, const AspeedPresentationSlot& active)
    {
        const SDL_FRect target = current_target_rect(active.width, active.height);
        const std::optional<AtenRemoteMousePosition> position = remote_mouse_position(
            event.wheel.mouse_x,
            event.wheel.mouse_y,
            target,
            active.width,
            active.height,
            mouse_buttons_ != 0);
        if (!position) {
            return;
        }

        const float y = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
            ? -event.wheel.y
            : event.wheel.y;
        if (y == 0.0f) {
            return;
        }

        const std::uint8_t wheel_mask = y > 0.0f ? 8U : 16U;
        queue_aten_pointer_event(*state_, position->x, position->y, wheel_mask);
        queue_aten_pointer_event(*state_, position->x, position->y, 0);
    }

    AtenViewOptions options_;
    std::shared_ptr<AtenViewState> state_;
    AspeedViewRenderer aspeed_;
    std::uint8_t mouse_buttons_ = 0;
    std::uint64_t last_mouse_motion_ticks_ = 0;
    AtenKeyDownState key_down_{};
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
