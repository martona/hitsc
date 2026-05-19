#include "aspeed_presenter.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hitsc {
namespace {

struct LockedTexture {
    SDL_Texture* texture = nullptr;

    ~LockedTexture()
    {
        unlock();
    }

    void unlock()
    {
        if (texture != nullptr) {
            SDL_UnlockTexture(texture);
            texture = nullptr;
        }
    }
};

void throw_sdl_error(std::string_view context)
{
    throw std::runtime_error(std::string(context) + ": " + SDL_GetError());
}

void destroy_slot(AspeedPresentationSlot& slot)
{
    if (slot.texture != nullptr) {
        SDL_DestroyTexture(slot.texture);
        slot.texture = nullptr;
    }
    slot.width = 0;
    slot.height = 0;
    slot.valid = false;
    slot.shadow.clear();
}

void ensure_slot_texture(SDL_Renderer* renderer, AspeedPresentationSlot& slot, int width, int height)
{
    if (slot.texture != nullptr && slot.width == width && slot.height == height) {
        return;
    }

    destroy_slot(slot);
    slot.texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING,
        width,
        height);
    if (slot.texture == nullptr) {
        throw_sdl_error("SDL_CreateTexture");
    }
    slot.width = width;
    slot.height = height;
}

void seed_framebuffer(
    std::uint8_t* target,
    std::size_t size,
    const AspeedPresentationSlot* previous_slot)
{
    if (previous_slot != nullptr && previous_slot->valid && previous_slot->shadow.size() == size) {
        std::memcpy(target, previous_slot->shadow.data(), size);
        return;
    }
    std::fill(target, target + size, 0xff);
}

bool try_decode_direct_to_texture(
    AspeedDecoder& decoder,
    int width,
    int height,
    const AspeedDecodeOptions& decode_options,
    const std::vector<std::uint8_t>& compressed,
    AspeedPresentationSlot& target_slot,
    const AspeedPresentationSlot* previous_slot)
{
    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(target_slot.texture, nullptr, &pixels, &pitch)) {
        return false;
    }

    LockedTexture locked{target_slot.texture};
    const int expected_pitch = width * 4;
    if (pitch != expected_pitch) {
        return false;
    }

    const std::size_t size = aspeed_frame_rgba_size(width, height);
    auto* rgba = static_cast<std::uint8_t*>(pixels);
    seed_framebuffer(rgba, size, previous_slot);
    decoder.decode_rgba_into(decode_options, compressed, std::span<std::uint8_t>(rgba, size));

    target_slot.shadow.resize(size);
    std::memcpy(target_slot.shadow.data(), rgba, size);
    target_slot.valid = true;
    locked.unlock();
    return true;
}

void decode_to_shadow_and_upload(
    AspeedDecoder& decoder,
    int width,
    int height,
    const AspeedDecodeOptions& decode_options,
    const std::vector<std::uint8_t>& compressed,
    AspeedPresentationSlot& target_slot,
    const AspeedPresentationSlot* previous_slot)
{
    const std::size_t size = aspeed_frame_rgba_size(width, height);
    target_slot.shadow.resize(size);
    seed_framebuffer(target_slot.shadow.data(), size, previous_slot);
    decoder.decode_rgba_into(
        decode_options,
        compressed,
        std::span<std::uint8_t>(target_slot.shadow.data(), target_slot.shadow.size()));

    if (!SDL_UpdateTexture(target_slot.texture, nullptr, target_slot.shadow.data(), width * 4)) {
        throw_sdl_error("SDL_UpdateTexture");
    }
    target_slot.valid = true;
}

} // namespace

AspeedPresenter::~AspeedPresenter()
{
    destroy();
}

void AspeedPresenter::destroy()
{
    for (AspeedPresentationSlot& slot : slots_) {
        destroy_slot(slot);
    }
    active_slot_ = -1;
}

bool AspeedPresenter::present(
    SDL_Renderer* renderer,
    AspeedDecoder& decoder,
    int width,
    int height,
    const AspeedDecodeOptions& decode_options,
    const std::vector<std::uint8_t>& compressed)
{
    if (active_slot_ >= 0 &&
        (slots_[active_slot_].width != width || slots_[active_slot_].height != height)) {
        destroy();
    }

    const int next_slot = active_slot_ == 0 ? 1 : 0;
    ensure_slot_texture(renderer, slots_[next_slot], width, height);

    const AspeedPresentationSlot* previous_slot = active_slot_ >= 0 ? &slots_[active_slot_] : nullptr;
    bool direct = false;
    try {
        direct = try_decode_direct_to_texture(
            decoder,
            width,
            height,
            decode_options,
            compressed,
            slots_[next_slot],
            previous_slot);
    } catch (...) {
        slots_[next_slot].valid = false;
        throw;
    }

    if (!direct) {
        decode_to_shadow_and_upload(
            decoder,
            width,
            height,
            decode_options,
            compressed,
            slots_[next_slot],
            previous_slot);
    }

    active_slot_ = next_slot;
    return direct;
}

const AspeedPresentationSlot* AspeedPresenter::active_slot() const
{
    if (active_slot_ < 0) {
        return nullptr;
    }
    return &slots_[active_slot_];
}

std::size_t aspeed_frame_rgba_size(int width, int height)
{
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
}

int aspeed_sampled_average_rgb(std::span<const std::uint8_t> rgba)
{
    if (rgba.empty()) {
        return 0;
    }

    std::uint64_t total = 0;
    std::uint64_t samples = 0;
    constexpr std::size_t stride_pixels = 257;
    for (std::size_t pixel = 0; pixel * 4 + 2 < rgba.size(); pixel += stride_pixels) {
        const std::size_t offset = pixel * 4;
        total += rgba[offset] + rgba[offset + 1] + rgba[offset + 2];
        ++samples;
    }
    return samples == 0 ? 0 : static_cast<int>(total / (samples * 3));
}

} // namespace hitsc
