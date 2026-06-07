#include "gui/viewer/viewer_paste_control.hpp"

#include "gui/toast.hpp"
#include "gui/viewer/keyboard_layout.hpp"

#include <QAction>
#include <QChar>
#include <QClipboard>
#include <QEnterEvent>
#include <QEvent>
#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>

#include <utility>
#include <vector>

namespace hitsc {
namespace {

constexpr int kButtonWidth = 46;
constexpr int kButtonHeight = 36;
constexpr int kChevronWidth = 14;

// Hand-drawn clipboard glyph (a board with a clip tab on top), matching the title bar's
// other hand-drawn glyphs so it stays crisp at any DPI without an icon-font dependency.
void draw_clipboard_glyph(QPainter& painter, const QRectF& bounds, const QColor& color)
{
    QPen pen(color, 1.4);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const double w = bounds.width();
    const double h = bounds.height();
    const QRectF board(bounds.left() + w * 0.18, bounds.top() + h * 0.14, w * 0.64, h * 0.74);
    painter.drawRoundedRect(board, 1.6, 1.6);
    const QRectF clip(bounds.center().x() - w * 0.15, bounds.top() + h * 0.02, w * 0.30, h * 0.18);
    painter.drawRoundedRect(clip, 1.2, 1.2);
}

// Small downward chevron, indicating the layout-picker drop-down.
void draw_chevron(QPainter& painter, const QRectF& bounds, const QColor& color)
{
    QPen pen(color, 1.3);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const QPointF c = bounds.center();
    const double s = 3.0;
    painter.drawPolyline(QPolygonF({
        QPointF(c.x() - s, c.y() - s / 2.0),
        QPointF(c.x(), c.y() + s / 2.0),
        QPointF(c.x() + s, c.y() - s / 2.0),
    }));
}

} // namespace

ViewerPasteControl::ViewerPasteControl(QWidget* parent)
    : QAbstractButton(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setToolTip(QStringLiteral("Type clipboard"));

    connect(this, &QAbstractButton::clicked, this, &ViewerPasteControl::do_paste);
}

ViewerPasteControl::~ViewerPasteControl() = default;

QSize ViewerPasteControl::sizeHint() const
{
    return QSize(kButtonWidth, kButtonHeight);
}

void ViewerPasteControl::set_toast_manager(ToastManager* toasts)
{
    toasts_ = toasts;
}

void ViewerPasteControl::apply_theme(bool dark)
{
    dark_ = dark;
    fg_neutral_ = dark ? QColor(0xF0, 0xF0, 0xF0) : QColor(0x1A, 0x1A, 0x1A);
    hover_bg_ = dark ? QColor(255, 255, 255, 25) : QColor(0, 0, 0, 20);
    update();
}

void ViewerPasteControl::set_layout(const QString& klid)
{
    std::unique_ptr<KeyboardLayout> engine = make_keyboard_layout(klid);
    if (engine) {
        klid_ = klid;
    } else {
        // Stored layout no longer installed (or none yet): fall back to the active one.
        const QString active = active_keyboard_layout_klid();
        engine = make_keyboard_layout(active);
        klid_ = engine ? active : QString();
    }
    layout_ = std::move(engine);
    setToolTip(layout_ ? QStringLiteral("Type clipboard - %1").arg(layout_->display_name())
                       : QStringLiteral("Type clipboard"));
    update();
}

bool ViewerPasteControl::in_chevron(const QPoint& pos) const
{
    return pos.x() >= width() - kChevronWidth;
}

void ViewerPasteControl::open_layout_menu()
{
    QMenu menu(this);
    const std::vector<LayoutInfo> layouts = enumerate_keyboard_layouts();
    if (layouts.empty()) {
        QAction* none = menu.addAction(tr("No keyboard layouts found"));
        none->setEnabled(false);
    }
    for (const LayoutInfo& info : layouts) {
        const QString label =
            info.is_ime ? tr("%1  (IME - unsupported)").arg(info.display_name) : info.display_name;
        QAction* action = menu.addAction(label);
        action->setCheckable(true);
        action->setChecked(!info.is_ime && info.klid == klid_);
        if (info.is_ime) {
            action->setEnabled(false);
            continue;
        }
        const QString klid = info.klid;
        connect(action, &QAction::triggered, this, [this, klid]() {
            set_layout(klid);
            emit layoutChanged(klid);
        });
    }
    menu.exec(mapToGlobal(QPoint(0, height())));
}

void ViewerPasteControl::do_paste()
{
    if (toasts_ == nullptr) {
        return;
    }

    const QClipboard* clipboard = QGuiApplication::clipboard();
    const QString text = clipboard != nullptr ? clipboard->text() : QString();
    if (text.isEmpty()) {
        toasts_->show(tr("Clipboard has no text"), ToastManager::Level::Info);
        return;
    }
    if (!layout_) {
        toasts_->show(tr("No keyboard layout selected"), ToastManager::Level::Error);
        return;
    }

    const TypeResult result = layout_->translate(text);
    if (!result.ok) {
        const TypeError& error = result.error;
        const QString cphex =
            QStringLiteral("%1").arg(static_cast<uint>(error.codepoint), 4, 16, QLatin1Char('0')).toUpper();
        QString glyph;
        if (error.codepoint <= 0xFFFF) {
            const QChar ch(static_cast<char16_t>(error.codepoint));
            if (ch.isPrint()) {
                glyph = QStringLiteral("'%1' ").arg(ch);
            }
        }
        toasts_->show(
            tr("Can't type %1(U+%2) - line %3, col %4 [%5]")
                .arg(glyph, cphex)
                .arg(error.line)
                .arg(error.column)
                .arg(layout_->display_name()),
            ToastManager::Level::Error,
            4200);
        return;
    }

    toasts_->show(
        tr("%1 chars -> %2 key events [%3]")
            .arg(result.plan.character_count)
            .arg(result.plan.key_event_count)
            .arg(layout_->display_name()),
        ToastManager::Level::Success);
}

void ViewerPasteControl::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (hovered_ || isDown()) {
        painter.fillRect(rect(), hover_bg_);
    }

    const QRectF glyph_area(0.0, 0.0, width() - kChevronWidth, height());
    const QRectF chevron_area(width() - kChevronWidth, 0.0, kChevronWidth, height());

    const double box = 16.0;
    const QPointF center = glyph_area.center();
    draw_clipboard_glyph(
        painter, QRectF(center.x() - box / 2.0, center.y() - box / 2.0, box, box), fg_neutral_);
    draw_chevron(painter, chevron_area, fg_neutral_);
}

void ViewerPasteControl::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && in_chevron(event->position().toPoint())) {
        open_layout_menu();
        return;  // chevron press opens the menu; it's not a paste click
    }
    QAbstractButton::mousePressEvent(event);
}

void ViewerPasteControl::enterEvent(QEnterEvent* event)
{
    hovered_ = true;
    update();
    QAbstractButton::enterEvent(event);
}

void ViewerPasteControl::leaveEvent(QEvent* event)
{
    hovered_ = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void ViewerPasteControl::clear_hover()
{
    if (hovered_) {
        hovered_ = false;
        update();
    }
}

} // namespace hitsc
