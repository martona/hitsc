#pragma once

#include "hardware_cursor.hpp"

#include <SDL3/SDL.h>

namespace hitsc {

class HardwareCursorPresenter {
public:
    HardwareCursorPresenter() = default;
    HardwareCursorPresenter(const HardwareCursorPresenter&) = delete;
    HardwareCursorPresenter& operator=(const HardwareCursorPresenter&) = delete;
    ~HardwareCursorPresenter();

    void destroy();
    void update(SDL_Renderer* renderer, const CursorImage& image);
    void render(
        SDL_Renderer* renderer,
        const HardwareCursor& cursor,
        const SDL_FRect& target,
        int frame_width,
        int frame_height) const;

private:
    SDL_Texture* texture_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

} // namespace hitsc
