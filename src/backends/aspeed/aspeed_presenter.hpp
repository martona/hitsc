#pragma once

#include <cstddef>

namespace hitsc {

// Byte size of a decoded ASPEED frame as RGBA (4 bytes/pixel).
std::size_t aspeed_frame_rgba_size(int width, int height);

} // namespace hitsc
