#pragma once

#include "aspeed_decoder.hpp"
#include "aspeed_presenter.hpp"
#include "hardware_cursor.hpp"
#include "hardware_cursor_presenter.hpp"
#include "log.hpp"
#include "view_base.hpp"

#include <SDL3/SDL.h>

#include <memory>
#include <string_view>

namespace hitsc {

class AspeedViewRenderer {
public:
    void destroy()
    {
        presenter_.destroy();
        cursor_presenter_.destroy();
    }

    void reset_sequences()
    {
        last_frame_sequence_ = 0;
        last_cursor_sequence_ = 0;
    }

    const AspeedPresentationSlot* active_slot() const
    {
        return presenter_.active_slot();
    }

    template <typename Frame, typename Cursor>
    void update(
        SDL_Renderer* renderer,
        LatestMailbox<Frame>& frames,
        LatestMailbox<Cursor>& cursors,
        std::string_view label,
        bool vverbose,
        bool& render_needed,
        bool& presented_new_frame)
    {
        bool cursor_texture_dirty = false;
        if (std::shared_ptr<const Cursor> cursor = cursors.latest(last_cursor_sequence_)) {
            hardware_cursor_ = *cursor;
            last_cursor_sequence_ = cursor->sequence;
            has_hardware_cursor_ = true;
            cursor_texture_dirty = true;
        }

        if (std::shared_ptr<const Frame> frame = frames.latest(last_frame_sequence_)) {
            last_frame_sequence_ = frame->sequence;
            const bool direct = presenter_.present(
                renderer,
                decoder_,
                frame->width,
                frame->height,
                frame->decode_options,
                frame->compressed);
            const AspeedPresentationSlot* active = presenter_.active_slot();
            ++presented_frames_;
            if (active != nullptr && vverbose && (presented_frames_ <= 20 || presented_frames_ % 60 == 0)) {
                log_info() << "presented " << label << " frame #" << presented_frames_
                           << " sequence=" << frame->sequence
                           << " size=" << frame->width << 'x' << frame->height
                           << " direct-texture=" << (direct ? "yes" : "no")
                           << " avg-rgb=" << aspeed_sampled_average_rgb(active->shadow);
            }
            render_needed = true;
            presented_new_frame = true;
            cursor_texture_dirty = has_hardware_cursor_;
        }

        if (cursor_texture_dirty && has_hardware_cursor_) {
            const AspeedPresentationSlot* active = presenter_.active_slot();
            if (active != nullptr) {
                const CursorImage cursor_image = make_cursor_image(
                    hardware_cursor_,
                    active->shadow,
                    active->width,
                    active->height);
                cursor_presenter_.update(renderer, cursor_image);
                render_needed = true;
            }
        }
    }

    void render(SDL_Renderer* renderer, const SDL_FRect& target) const
    {
        const AspeedPresentationSlot* active = presenter_.active_slot();
        if (active == nullptr || active->texture == nullptr) {
            return;
        }

        SDL_RenderTexture(renderer, active->texture, nullptr, &target);
        if (has_hardware_cursor_) {
            cursor_presenter_.render(
                renderer,
                hardware_cursor_,
                target,
                active->width,
                active->height);
        }
    }

private:
    AspeedDecoder decoder_;
    AspeedPresenter presenter_;
    HardwareCursorPresenter cursor_presenter_;
    HardwareCursor hardware_cursor_;
    bool has_hardware_cursor_ = false;
    int presented_frames_ = 0;
    std::uint64_t last_frame_sequence_ = 0;
    std::uint64_t last_cursor_sequence_ = 0;
};

} // namespace hitsc
