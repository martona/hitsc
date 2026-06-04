#include "screen_geometry.hpp"

#include <QGuiApplication>
#include <QScreen>

namespace hitsc {

bool rect_within_virtual_desktop(const QRect& rect)
{
    if (rect.width() <= 0 || rect.height() <= 0) {
        return false;
    }

    QRect virtual_desktop;
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen == nullptr) {
            continue;
        }
        virtual_desktop = virtual_desktop.isNull()
            ? screen->geometry()
            : virtual_desktop.united(screen->geometry());
    }

    return !virtual_desktop.isNull() && virtual_desktop.contains(rect);
}

} // namespace hitsc
