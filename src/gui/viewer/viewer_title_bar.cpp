#include "gui/viewer/viewer_title_bar.hpp"

#include <QAbstractButton>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QSizePolicy>

namespace hitsc {

// A single Win11-style caption button (minimize / maximize / restore / close),
// painted by hand so the glyphs stay crisp at any DPI and the close button gets the
// red hover. No Q_OBJECT: it only overrides paintEvent/sizeHint and reuses
// QAbstractButton::clicked().
class CaptionButton : public QAbstractButton {
public:
    enum class Glyph { Minimize, Maximize, Restore, Close };

    explicit CaptionButton(Glyph glyph, QWidget* parent = nullptr)
        : QAbstractButton(parent)
        , glyph_(glyph)
    {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::ArrowCursor);
        setAttribute(Qt::WA_Hover, true);
    }

    void set_glyph(Glyph glyph)
    {
        if (glyph_ != glyph) {
            glyph_ = glyph;
            update();
        }
    }

    void set_colors(const QColor& fg, const QColor& hover_bg, bool is_close)
    {
        fg_ = fg;
        hover_bg_ = hover_bg;
        is_close_ = is_close;
        update();
    }

    QSize sizeHint() const override { return QSize(46, 36); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const bool hovered = underMouse() || isDown();
        if (hovered) {
            QColor bg = hover_bg_;
            if (isDown()) {
                bg = is_close_ ? QColor(0xB0, 0x26, 0x19) : bg.darker(115);
            }
            p.fillRect(rect(), bg);
        }

        QColor fg = fg_;
        if (is_close_ && hovered) {
            fg = Qt::white;
        }

        // ~10px glyph box centered in the button, drawn in device-independent
        // coordinates so it scales with the display.
        const qreal s = 10.0;
        const QPointF c(width() / 2.0, height() / 2.0);
        const QRectF g(c.x() - s / 2.0, c.y() - s / 2.0, s, s);

        QPen pen(fg, 1.0);
        p.setPen(pen);

        switch (glyph_) {
        case Glyph::Minimize:
            p.drawLine(QPointF(g.left(), c.y()), QPointF(g.right(), c.y()));
            break;
        case Glyph::Maximize:
            p.drawRect(g);
            break;
        case Glyph::Restore: {
            // Two offset squares: the back one is drawn as only its visible top and
            // right edges (the rest is covered by the front square), so no masking.
            const qreal o = 2.0;
            const QRectF back(g.left() + o, g.top(), g.width() - o, g.height() - o);
            p.drawLine(back.topLeft(), back.topRight());
            p.drawLine(back.topRight(), back.bottomRight());
            const QRectF front(g.left(), g.top() + o, g.width() - o, g.height() - o);
            p.drawRect(front);
            break;
        }
        case Glyph::Close:
            p.drawLine(g.topLeft(), g.bottomRight());
            p.drawLine(g.topRight(), g.bottomLeft());
            break;
        }
    }

private:
    Glyph glyph_;
    QColor fg_ = Qt::white;
    QColor hover_bg_ = QColor(255, 255, 255, 25);
    bool is_close_ = false;
};

namespace {
constexpr int kBarHeight = 36;
constexpr int kButtonWidth = 46;
} // namespace

ViewerTitleBar::ViewerTitleBar(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(kBarHeight);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    icon_ = new QLabel(this);
    icon_->setFixedSize(kBarHeight, kBarHeight);
    icon_->setAlignment(Qt::AlignCenter);
    icon_->setPixmap(QIcon(QStringLiteral(":/icons/hitsc-16.png")).pixmap(16, 16));
    row->addWidget(icon_);

    title_ = new QLabel(this);
    title_->setIndent(4);
    title_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // The title label spans the drag region; let mouse events fall through to
    // QWindowKit so the window drags when grabbed anywhere on it.
    title_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    row->addWidget(title_, 1);

    action_area_ = new QHBoxLayout();
    action_area_->setContentsMargins(0, 0, 0, 0);
    action_area_->setSpacing(0);
    row->addLayout(action_area_);

    min_ = new CaptionButton(CaptionButton::Glyph::Minimize, this);
    max_ = new CaptionButton(CaptionButton::Glyph::Maximize, this);
    close_ = new CaptionButton(CaptionButton::Glyph::Close, this);
    min_->setFixedSize(kButtonWidth, kBarHeight);
    max_->setFixedSize(kButtonWidth, kBarHeight);
    close_->setFixedSize(kButtonWidth, kBarHeight);
    row->addWidget(min_);
    row->addWidget(max_);
    row->addWidget(close_);

    connect(min_, &QAbstractButton::clicked, this, &ViewerTitleBar::minimizeRequested);
    connect(max_, &QAbstractButton::clicked, this, &ViewerTitleBar::maximizeRestoreRequested);
    connect(close_, &QAbstractButton::clicked, this, &ViewerTitleBar::closeRequested);

    apply_theme(false);  // ViewerWindow re-applies with the real color scheme
}

void ViewerTitleBar::set_title(const QString& text)
{
    full_title_ = text;
    update_elided_title();
}

void ViewerTitleBar::set_maximized(bool maximized)
{
    if (maximized_ == maximized) {
        return;
    }
    maximized_ = maximized;
    max_->set_glyph(maximized ? CaptionButton::Glyph::Restore : CaptionButton::Glyph::Maximize);
}

void ViewerTitleBar::apply_theme(bool dark)
{
    bar_bg_ = dark ? QColor(32, 32, 32) : QColor(243, 243, 243);
    separator_ = dark ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 20);

    const QColor fg = dark ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x1A, 0x1A, 0x1A);
    const QColor neutral_hover = dark ? QColor(255, 255, 255, 25) : QColor(0, 0, 0, 20);
    const QColor close_hover(0xC4, 0x2B, 0x1C);

    title_->setStyleSheet(QStringLiteral("color: %1;").arg(fg.name()));
    min_->set_colors(fg, neutral_hover, false);
    max_->set_colors(fg, neutral_hover, false);
    close_->set_colors(fg, close_hover, true);
    update();
}

QWidget* ViewerTitleBar::icon_button() const { return icon_; }
QWidget* ViewerTitleBar::min_button() const { return min_; }
QWidget* ViewerTitleBar::max_button() const { return max_; }
QWidget* ViewerTitleBar::close_button() const { return close_; }

void ViewerTitleBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), bar_bg_);
    p.fillRect(0, height() - 1, width(), 1, separator_);
}

void ViewerTitleBar::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    update_elided_title();
}

void ViewerTitleBar::update_elided_title()
{
    if (title_ == nullptr) {
        return;
    }
    const int available = qMax(0, title_->width() - 2 * title_->indent());
    title_->setText(title_->fontMetrics().elidedText(full_title_, Qt::ElideRight, available));
}

} // namespace hitsc
