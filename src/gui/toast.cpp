#include "gui/toast.hpp"

#include <QColor>
#include <QEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QPropertyAnimation>
#include <QRect>
#include <QTimer>
#include <QWidget>

#include <algorithm>

namespace hitsc {
namespace {

constexpr int kPadX = 14;
constexpr int kPadY = 8;
constexpr int kMaxWidth = 460;
constexpr int kBottomMargin = 26;
constexpr int kGap = 8;
constexpr int kFadeInMs = 140;
constexpr int kFadeOutMs = 260;

QColor level_background(ToastManager::Level level)
{
    switch (level) {
    case ToastManager::Level::Success:
        return QColor(0x1E, 0x7D, 0x32);
    case ToastManager::Level::Error:
        return QColor(0xB3, 0x26, 0x1E);
    case ToastManager::Level::Info:
        break;
    }
    return QColor(0x30, 0x30, 0x30);
}

// One transient panel. A frameless, translucent, non-activating top-level window that
// fades itself in, lingers, then fades out and self-deletes (WA_DeleteOnClose).
class ToastWidget : public QWidget {
public:
    ToastWidget(const QString& text, ToastManager::Level level, int duration_ms)
        : QWidget(nullptr,
                  Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                      Qt::WindowDoesNotAcceptFocus)
        , text_(text)
        , background_(level_background(level))
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_DeleteOnClose);
        setFocusPolicy(Qt::NoFocus);

        const QFontMetrics metrics(font());
        const int text_width = metrics.horizontalAdvance(text_);
        const int width = std::min(text_width + 2 * kPadX, kMaxWidth);
        resize(std::max(width, 80), metrics.height() + 2 * kPadY);

        setWindowOpacity(0.0);

        auto* fade_in = new QPropertyAnimation(this, "windowOpacity", this);
        fade_in->setDuration(kFadeInMs);
        fade_in->setStartValue(0.0);
        fade_in->setEndValue(1.0);
        fade_in->start(QAbstractAnimation::DeleteWhenStopped);

        QTimer::singleShot(duration_ms, this, [this] { fade_out(); });
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(background_);
        const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.drawRoundedRect(bounds, 6.0, 6.0);

        painter.setPen(QColor(0xF5, 0xF5, 0xF5));
        painter.drawText(rect(), Qt::AlignCenter, text_);
    }

private:
    void fade_out()
    {
        auto* fade = new QPropertyAnimation(this, "windowOpacity", this);
        fade->setDuration(kFadeOutMs);
        fade->setStartValue(windowOpacity());
        fade->setEndValue(0.0);
        connect(fade, &QPropertyAnimation::finished, this, &QWidget::close);
        fade->start(QAbstractAnimation::DeleteWhenStopped);
    }

    QString text_;
    QColor background_;
};

} // namespace

ToastManager::ToastManager(QWidget* anchor, QObject* parent)
    : QObject(parent)
    , anchor_(anchor)
{
    if (anchor_) {
        anchor_->installEventFilter(this);
    }
}

void ToastManager::show(const QString& text, Level level, int duration_ms)
{
    if (!anchor_) {
        return;
    }

    auto* toast = new ToastWidget(text, level, duration_ms);
    toasts_.push_back(toast);
    connect(toast, &QObject::destroyed, this, [this] { reflow(); });

    reflow();
    toast->show();
}

bool ToastManager::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == anchor_ &&
        (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        reflow();
    }
    return QObject::eventFilter(watched, event);
}

void ToastManager::reflow()
{
    toasts_.erase(
        std::remove_if(toasts_.begin(), toasts_.end(),
                       [](const QPointer<QWidget>& toast) { return toast.isNull(); }),
        toasts_.end());

    if (!anchor_) {
        return;
    }

    // Newest at the bottom; older ones stack upward.
    const QRect anchor_geometry = anchor_->frameGeometry();
    int y = anchor_geometry.bottom() - kBottomMargin;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        QWidget* toast = *it;
        if (toast == nullptr) {
            continue;
        }
        y -= toast->height();
        toast->move(anchor_geometry.center().x() - toast->width() / 2, y);
        y -= kGap;
    }
}

} // namespace hitsc
