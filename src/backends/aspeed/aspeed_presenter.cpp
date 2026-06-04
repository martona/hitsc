#include "aspeed_presenter.hpp"

namespace hitsc {

std::size_t aspeed_frame_rgba_size(int width, int height)
{
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
}

} // namespace hitsc
