#pragma once

#include "aspeed_decoder.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace hitsc {

struct AspeedPresentationSlot {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
    bool valid = false;
    std::vector<std::uint8_t> shadow;
};

class AspeedPresenter {
public:
    AspeedPresenter() = default;
    ~AspeedPresenter();

    AspeedPresenter(const AspeedPresenter&) = delete;
    AspeedPresenter& operator=(const AspeedPresenter&) = delete;

    void destroy();

    bool present(
        SDL_Renderer* renderer,
        AspeedDecoder& decoder,
        int width,
        int height,
        const AspeedDecodeOptions& decode_options,
        const std::vector<std::uint8_t>& compressed);

    const AspeedPresentationSlot* active_slot() const;

private:
    std::array<AspeedPresentationSlot, 2> slots_;
    int active_slot_ = -1;
};

std::size_t aspeed_frame_rgba_size(int width, int height);
int aspeed_sampled_average_rgb(std::span<const std::uint8_t> rgba);

} // namespace hitsc
