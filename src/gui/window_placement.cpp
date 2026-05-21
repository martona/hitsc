#include "window_placement.hpp"

#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

#include <utility>

namespace hitsc {

WindowPlacementController::WindowPlacementController(
    QWindow* window,
    WindowPrefsStore store,
    QString preference_name,
    QObject* parent)
    : QObject(parent)
    , window_(window)
    , store_(std::move(store))
    , preference_name_(std::move(preference_name))
{
    if (window_ != nullptr) {
        window_->installEventFilter(this);
    }
}

WindowPlacementController::~WindowPlacementController()
{
    if (window_ != nullptr) {
        window_->removeEventFilter(this);
    }
}

void WindowPlacementController::restore()
{
    if (window_ == nullptr) {
        return;
    }

    const auto rect = store_.load_window_rect(preference_name_);
    if (rect && is_rect_within_virtual_desktop(*rect)) {
        window_->setGeometry(*rect);
    }
}

bool WindowPlacementController::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != window_ || event == nullptr) {
        return QObject::eventFilter(watched, event);
    }

    if (event->type() == QEvent::Close) {
        save_if_visible();
    } else if (event->type() == QEvent::Destroy) {
        window_ = nullptr;
    }

    return QObject::eventFilter(watched, event);
}

bool WindowPlacementController::is_rect_within_virtual_desktop(const QRect& rect)
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

void WindowPlacementController::save_if_visible()
{
    if (window_ == nullptr) {
        return;
    }

    if (window_->visibility() == QWindow::Minimized
        || window_->windowStates().testFlag(Qt::WindowMinimized)) {
        return;
    }

    store_.save_window_rect(preference_name_, window_->geometry());
}

} // namespace hitsc
