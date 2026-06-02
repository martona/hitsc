#include "window_placement.hpp"

#include <QEvent>
#include <QGuiApplication>
#include <QPoint>
#include <QScreen>
#include <QSize>
#include <QWindow>

#include <utility>

namespace hitsc {
namespace {

constexpr int kWindowSizeMax = 16777215;

const QSize kMiniSize(440, 376);
const QSize kExpandedMinSize(360, 320);
const QSize kExpandedDefaultSize(980, 680);

QString normalize_mode(const QString& mode)
{
    return mode == QStringLiteral("mini") ? QStringLiteral("mini") : QStringLiteral("expanded");
}

QRect centered_rect(const QSize& size)
{
    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        return QRect(QPoint(120, 120), size);
    }
    const QRect available = screen->availableGeometry();
    return QRect(
        available.center() - QPoint(size.width() / 2, size.height() / 2),
        size);
}

} // namespace

WindowPlacementController::WindowPlacementController(
    WindowPrefsStore store,
    QString preference_name,
    QObject* parent)
    : QObject(parent)
    , store_(std::move(store))
    , preference_name_(std::move(preference_name))
    , mode_(QStringLiteral("expanded"))
{
}

WindowPlacementController::~WindowPlacementController()
{
    if (window_ != nullptr) {
        window_->removeEventFilter(this);
    }
}

void WindowPlacementController::attach(QWindow* window)
{
    if (window_ != nullptr) {
        window_->removeEventFilter(this);
    }
    window_ = window;
    if (window_ != nullptr) {
        window_->installEventFilter(this);
    }
}

QString WindowPlacementController::mode() const
{
    return mode_;
}

void WindowPlacementController::restore()
{
    if (window_ == nullptr) {
        return;
    }

    const auto saved_mode = store_.load_string(preference_name_ + QStringLiteral(".Mode"));
    mode_ = normalize_mode(saved_mode ? *saved_mode : QString());
    apply_mode_geometry(mode_);
    emit modeChanged();
}

void WindowPlacementController::save()
{
    save_if_visible();
}

void WindowPlacementController::setMode(const QString& mode)
{
    const QString normalized = normalize_mode(mode);
    if (normalized == mode_) {
        return;
    }

    // Persist the current mode's geometry before switching away from it.
    save_if_visible();
    mode_ = normalized;
    store_.save_string(preference_name_ + QStringLiteral(".Mode"), mode_);
    apply_mode_geometry(mode_);
    emit modeChanged();
}

QString WindowPlacementController::rect_key(const QString& mode) const
{
    return mode == QStringLiteral("mini")
        ? preference_name_ + QStringLiteral(".Mini")
        : preference_name_;
}

void WindowPlacementController::apply_mode_geometry(const QString& mode)
{
    if (window_ == nullptr) {
        return;
    }

    const auto saved = store_.load_window_rect(rect_key(mode));

    if (mode == QStringLiteral("mini")) {
        // Fixed-size, non-resizable mini window; only the position is restored.
        const QRect target =
            (saved && is_rect_within_virtual_desktop(QRect(saved->topLeft(), kMiniSize)))
            ? QRect(saved->topLeft(), kMiniSize)
            : centered_rect(kMiniSize);
        window_->setMinimumSize(kMiniSize);
        window_->setMaximumSize(kMiniSize);
        window_->setGeometry(target);
    } else {
        window_->setMaximumSize(QSize(kWindowSizeMax, kWindowSizeMax));
        window_->setMinimumSize(kExpandedMinSize);
        if (saved && is_rect_within_virtual_desktop(*saved)) {
            window_->setGeometry(*saved);
        } else {
            window_->setGeometry(centered_rect(kExpandedDefaultSize));
        }
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

    store_.save_window_rect(rect_key(mode_), window_->geometry());
}

} // namespace hitsc
