#include "gui/viewer/viewer_surface.hpp"

#include "gui/viewer/qt_console.hpp"

#include <QColor>
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <QRect>
#include <QWheelEvent>

#include <algorithm>
#include <optional>

namespace hitsc {
namespace {

std::optional<KvmMouseButton> kvm_button(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton:
        return KvmMouseButton::LEFT;
    case Qt::MiddleButton:
        return KvmMouseButton::MIDDLE;
    case Qt::RightButton:
        return KvmMouseButton::RIGHT;
    case Qt::BackButton:
        return KvmMouseButton::X1;
    case Qt::ForwardButton:
        return KvmMouseButton::X2;
    default:
        return std::nullopt;
    }
}

KvmPointerPos to_pos(const QPointF& point)
{
    return KvmPointerPos{static_cast<float>(point.x()), static_cast<float>(point.y())};
}

} // namespace

ViewerSurface::ViewerSurface(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
}

void ViewerSurface::show_console(const ConsoleScreen& screen)
{
    console_active_ = true;
    console_ = screen;
    update();
}

void ViewerSurface::show_frame(const QImage& frame)
{
    console_active_ = false;
    frame_ = frame;
    update();
}

void ViewerSurface::paintEvent(QPaintEvent*)
{
    QPainter painter(this);

    if (console_active_) {
        render_console_qpainter(painter, size(), console_);
        return;
    }

    painter.fillRect(rect(), QColor(12, 14, 18));
    if (frame_.isNull() || frame_.width() <= 0 || frame_.height() <= 0) {
        return;
    }

    // Aspect-preserving fit, centered (mirrors KvmViewBase::centered_target_rect).
    const double scale = std::min(
        static_cast<double>(width()) / frame_.width(),
        static_cast<double>(height()) / frame_.height());
    const int w = static_cast<int>(frame_.width() * scale);
    const int h = static_cast<int>(frame_.height() * scale);
    const QRect target((width() - w) / 2, (height() - h) / 2, w, h);
    painter.drawImage(target, frame_);
}

void ViewerSurface::mousePressEvent(QMouseEvent* event)
{
    if (const auto button = kvm_button(event->button())) {
        emit pointerButton(KvmPointerButton{*button, true, to_pos(event->position())});
    }
}

void ViewerSurface::mouseReleaseEvent(QMouseEvent* event)
{
    if (const auto button = kvm_button(event->button())) {
        emit pointerButton(KvmPointerButton{*button, false, to_pos(event->position())});
    }
}

void ViewerSurface::mouseMoveEvent(QMouseEvent* event)
{
    emit pointerMotion(KvmPointerMotion{to_pos(event->position())});
}

void ViewerSurface::wheelEvent(QWheelEvent* event)
{
    const QPoint steps = event->angleDelta();
    emit pointerWheel(KvmPointerWheel{
        static_cast<float>(steps.x()) / 120.0f,
        static_cast<float>(steps.y()) / 120.0f,
        to_pos(event->position())});
}

void ViewerSurface::focusOutEvent(QFocusEvent*)
{
    emit focusLost();
}

} // namespace hitsc
