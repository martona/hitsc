#pragma once

#include <QRect>

namespace hitsc {

// True if `rect` lies entirely within the union of the connected screens'
// geometries (so it would open on-screen). False for an empty rect or when no
// screens are present. Used to reject a saved window position whose monitor is
// gone, so the window falls back to a default placement instead of off-screen.
bool rect_within_virtual_desktop(const QRect& rect);

} // namespace hitsc
