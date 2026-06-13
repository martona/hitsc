#include "gui/viewer/viewer_cd_control.hpp"

#include <QEnterEvent>
#include <QEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRectF>

namespace hitsc {
namespace {

constexpr int kButtonWidth = 46;
constexpr int kButtonHeight = 36;

// Hand-drawn optical-disc glyph (a rim, a center hub, and a short highlight arc), matching
// the title bar's other hand-drawn glyphs so it stays crisp at any DPI without an icon-font
// dependency.
void draw_disc_glyph(QPainter& painter, const QRectF& bounds, const QColor& color)
{
    QPen pen(color, 1.4);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const QPointF c = bounds.center();
    const double r = qMin(bounds.width(), bounds.height()) * 0.46;
    painter.drawEllipse(c, r, r);  // disc rim

    const double hub = r * 0.30;
    painter.drawEllipse(c, hub, hub);  // center hub / hole

    // A short arc on the upper-left suggests a shiny disc surface.
    const double rm = r * 0.66;
    const QRectF arc(c.x() - rm, c.y() - rm, rm * 2.0, rm * 2.0);
    painter.drawArc(arc, 100 * 16, 60 * 16);
}

} // namespace

ViewerCdControl::ViewerCdControl(QWidget* parent)
    : QAbstractButton(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setToolTip(QStringLiteral("Mount CD / ISO"));

    connect(this, &QAbstractButton::clicked, this, [this] {
        if (mount_enabled_) {
            emit mountRequested();
        }
    });
}

QSize ViewerCdControl::sizeHint() const
{
    return QSize(kButtonWidth, kButtonHeight);
}

void ViewerCdControl::apply_theme(bool dark)
{
    dark_ = dark;
    fg_neutral_ = dark ? QColor(0xF0, 0xF0, 0xF0) : QColor(0x1A, 0x1A, 0x1A);
    hover_bg_ = dark ? QColor(255, 255, 255, 25) : QColor(0, 0, 0, 20);
    update();
}

void ViewerCdControl::set_mount_enabled(bool enabled)
{
    if (mount_enabled_ == enabled) {
        return;
    }
    mount_enabled_ = enabled;
    update();
}

void ViewerCdControl::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (hovered_ || isDown()) {
        painter.fillRect(rect(), hover_bg_);
    }

    QColor glyph_color = fg_neutral_;
    if (!mount_enabled_) {
        glyph_color.setAlphaF(0.35f);  // dimmed: no connected guest to mount into
    }

    const double box = 16.0;
    const QPointF center(width() / 2.0, height() / 2.0);
    draw_disc_glyph(
        painter, QRectF(center.x() - box / 2.0, center.y() - box / 2.0, box, box), glyph_color);
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
