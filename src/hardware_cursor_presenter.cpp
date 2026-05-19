#include "hardware_cursor_presenter.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace hitsc {
namespace {

void throw_sdl_cursor_error(std::string_view context)
{
    throw std::runtime_error(std::string(context) + ": " + SDL_GetError());
}

} // namespace

HardwareCursorPresenter::~HardwareCursorPresenter()
{
    destroy();
}

void HardwareCursorPresenter::destroy()
{
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
}

void HardwareCursorPresenter::update(SDL_Renderer* renderer, const CursorImage& image)
{
    if (image.rgba.empty() || image.width <= 0 || image.height <= 0) {
        destroy();
        return;
    }

    if (texture_ == nullptr || width_ != image.width || height_ != image.height) {
        destroy();
        texture_ = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING,
            image.width,
            image.height);
        if (texture_ == nullptr) {
            throw_sdl_cursor_error("SDL_CreateTexture(cursor)");
        }
        SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND);
        width_ = image.width;
        height_ = image.height;
    }

    if (!SDL_UpdateTexture(texture_, nullptr, image.rgba.data(), image.width * 4)) {
        throw_sdl_cursor_error("SDL_UpdateTexture(cursor)");
    }
}

void HardwareCursorPresenter::render(
    SDL_Renderer* renderer,
    const HardwareCursor& cursor,
    const SDL_FRect& target,
    int frame_width,
    int frame_height) const
{
    if (texture_ == nullptr || !cursor.visible || frame_width <= 0 || frame_height <= 0) {
        return;
    }

    const float scale_x = target.w / static_cast<float>(frame_width);
    const float scale_y = target.h / static_cast<float>(frame_height);
    SDL_FRect cursor_target{};
    cursor_target.x = target.x + static_cast<float>(cursor.x) * scale_x;
    cursor_target.y = target.y + static_cast<float>(cursor.y) * scale_y;
    cursor_target.w = static_cast<float>(width_) * scale_x;
    cursor_target.h = static_cast<float>(height_) * scale_y;
    SDL_RenderTexture(renderer, texture_, nullptr, &cursor_target);
}

} // namespace hitsc
