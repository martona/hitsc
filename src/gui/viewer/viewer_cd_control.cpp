#include "gui/viewer/viewer_cd_control.hpp"

#include "gui/toast.hpp"
#include "virtual_media/virtual_media.hpp"

#include <QEnterEvent>
#include <QEvent>
#include <QFileDialog>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTimer>

#include <cmath>
#include <string>
#include <vector>

namespace hitsc {
namespace {

constexpr int kButtonWidth = 46;
constexpr int kButtonHeight = 36;

// Hand-drawn optical-disc glyph (rim, hub, a highlight arc), matching the title bar's other
// hand-drawn glyphs. Filled (translucent) when a disc is mounted.
void draw_disc_glyph(QPainter& painter, const QRectF& bounds, const QColor& color, bool filled)
{
    QPen pen(color, 1.4);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);

    const QPointF c = bounds.center();
    const double r = qMin(bounds.width(), bounds.height()) * 0.46;

    if (filled) {
        QColor fill = color;
        fill.setAlphaF(0.25f);
        painter.setBrush(fill);
    } else {
        painter.setBrush(Qt::NoBrush);
    }
    painter.drawEllipse(c, r, r);  // disc rim

    painter.setBrush(Qt::NoBrush);
    const double hub = r * 0.30;
    painter.drawEllipse(c, hub, hub);  // center hub / hole

    const double rm = r * 0.66;
    const QRectF arc(c.x() - rm, c.y() - rm, rm * 2.0, rm * 2.0);
    painter.drawArc(arc, 100 * 16, 60 * 16);  // shiny-surface highlight
}

} // namespace

ViewerCdControl::ViewerCdControl(QWidget* parent)
    : QAbstractButton(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setVisible(false);  // shown once a controller is bound
    refresh_tooltip();

    // Poll the controller for state changes + outcome toasts (it's fed by the media thread).
    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(400);
    connect(poll_timer_, &QTimer::timeout, this, &ViewerCdControl::poll);

    // Glyph pulse while a mount is in flight.
    pulse_timer_ = new QTimer(this);
    pulse_timer_->setInterval(33);
    connect(pulse_timer_, &QTimer::timeout, this, [this] {
        pulse_phase_ += 0.18;
        update();
    });

    connect(this, &QAbstractButton::clicked, this, &ViewerCdControl::on_clicked);
}

QSize ViewerCdControl::sizeHint() const
{
    return QSize(kButtonWidth, kButtonHeight);
}

void ViewerCdControl::set_toast_manager(ToastManager* toasts)
{
    toasts_ = toasts;
}

void ViewerCdControl::set_controller(VirtualMediaController* controller)
{
    controller_ = controller;
    if (controller_ != nullptr) {
        set_state(controller_->state());
        poll_timer_->start();
        setVisible(true);
    } else {
        poll_timer_->stop();
        pulse_timer_->stop();
        state_ = MediaState::Idle;
        setVisible(false);
    }
    refresh_tooltip();
    update();
}

void ViewerCdControl::apply_theme(bool dark)
{
    dark_ = dark;
    fg_neutral_ = dark ? QColor(0xF0, 0xF0, 0xF0) : QColor(0x1A, 0x1A, 0x1A);
    hover_bg_ = dark ? QColor(255, 255, 255, 25) : QColor(0, 0, 0, 20);
    update();
}

void ViewerCdControl::poll()
{
    if (controller_ == nullptr) {
        return;
    }
    for (const MediaOutcome& outcome : controller_->take_outcomes()) {
        if (toasts_ != nullptr) {
            toasts_->show(
                QString::fromStdString(outcome.detail),
                outcome.ok ? ToastManager::Level::Info : ToastManager::Level::Error);
        }
    }
    set_state(controller_->state());
}

void ViewerCdControl::set_state(MediaState state)
{
    if (state_ == state) {
        return;
    }
    state_ = state;
    if (state_ == MediaState::Mounting) {
        pulse_phase_ = 0.0;
        pulse_timer_->start();
    } else {
        pulse_timer_->stop();
    }
    refresh_tooltip();
    update();
}

void ViewerCdControl::on_clicked()
{
    if (controller_ == nullptr) {
        return;
    }
    switch (state_) {
    case MediaState::Mounted:
        controller_->unmount();
        break;
    case MediaState::Mounting:
        break;  // a mount is already in flight; ignore the click
    case MediaState::Idle:
    case MediaState::Error: {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Mount ISO as virtual CD"), QString(),
            tr("Disc images (*.iso *.img);;All files (*)"));
        if (!path.isEmpty()) {
            controller_->mount(path.toStdString());
            set_state(MediaState::Mounting);  // optimistic; poll() confirms/corrects from the channel
        }
        break;
    }
    }
}

void ViewerCdControl::refresh_tooltip()
{
    switch (state_) {
    case MediaState::Mounting:
        setToolTip(tr("Mounting virtual CD..."));
        break;
    case MediaState::Mounted:
        setToolTip(tr("Virtual CD mounted - click to eject"));
        break;
    case MediaState::Idle:
    case MediaState::Error:
    default:
        setToolTip(tr("Mount ISO as virtual CD"));
        break;
    }
}

void ViewerCdControl::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (hovered_ || isDown()) {
        painter.fillRect(rect(), hover_bg_);
    }

    QColor color = fg_neutral_;
    bool filled = false;
    switch (state_) {
    case MediaState::Mounted:
        color = QColor(0x3F, 0xB9, 0x50);  // green = a disc is mounted
        filled = true;
        break;
    case MediaState::Mounting: {
        const double level = 0.35 + 0.65 * (0.5 + 0.5 * std::sin(pulse_phase_));
        color.setAlphaF(static_cast<float>(level));
        break;
    }
    case MediaState::Error:
        color = QColor(0xC4, 0x3A, 0x31);  // red = last mount failed (the toast carries the detail)
        break;
    case MediaState::Idle:
        break;
    }

    const double box = 16.0;
    const QPointF center(width() / 2.0, height() / 2.0);
    draw_disc_glyph(
        painter, QRectF(center.x() - box / 2.0, center.y() - box / 2.0, box, box), color, filled);
}

void ViewerCdControl::enterEvent(QEnterEvent* event)
{
    hovered_ = true;
    update();
    QAbstractButton::enterEvent(event);
}

void ViewerCdControl::leaveEvent(QEvent* event)
{
    hovered_ = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void ViewerCdControl::clear_hover()
{
    if (hovered_) {
        hovered_ = false;
        update();
    }
}

} // namespace hitsc
