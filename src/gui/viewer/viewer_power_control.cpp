#include "gui/viewer/viewer_power_control.hpp"

#include "gui/toast.hpp"

#include <QAbstractAnimation>
#include <QEnterEvent>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPoint>
#include <QRectF>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <utility>
#include <vector>

namespace hitsc {
namespace {

constexpr int kButtonWidth = 46;
constexpr int kButtonHeight = 36;
constexpr double kHoldMs = 1200.0;

// A press-and-hold button: holding fills a bar over ~1.2s and fires on completion;
// releasing early cancels. The "gaming UX" confirmation in place of a message box.
class HoldButton : public QAbstractButton {
public:
    HoldButton(const QString& label, QColor accent, std::function<void()> on_complete, QWidget* parent)
        : QAbstractButton(parent)
        , accent_(accent)
        , on_complete_(std::move(on_complete))
    {
        setText(label);
        setToolTip(QStringLiteral("Hold to confirm: %1").arg(label));
        setFixedHeight(32);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        timer_ = new QTimer(this);
        timer_->setInterval(16);
        connect(timer_, &QTimer::timeout, this, [this] { advance(); });
    }

    void set_theme(bool dark)
    {
        base_ = dark ? QColor(0x3A, 0x3A, 0x3A) : QColor(0xE6, 0xE6, 0xE6);
        text_color_ = dark ? QColor(0xF0, 0xF0, 0xF0) : QColor(0x20, 0x20, 0x20);
        update();
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            holding_ = true;
            progress_ = 0.0;
            timer_->start();
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent*) override { cancel(); }
    void leaveEvent(QEvent*) override { cancel(); }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool enabled = isEnabled();

        QPainterPath clip;
        clip.addRoundedRect(bounds, 5.0, 5.0);
        painter.setClipPath(clip);

        QColor base = base_;
        if (!enabled) {
            base.setAlpha(110);  // greyed when the action doesn't apply to the state
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(base);
        painter.drawRect(bounds);

        if (enabled && progress_ > 0.0) {
            QColor fill = accent_;
            fill.setAlpha(holding_ ? 230 : 150);
            QRectF filled = bounds;
            filled.setWidth(bounds.width() * progress_);
            painter.setBrush(fill);
            painter.drawRect(filled);
        }

        painter.setClipping(false);
        QColor fg = text_color_;
        if (!enabled) {
            fg.setAlpha(110);
        }
        painter.setPen(fg);
        painter.drawText(rect(), Qt::AlignCenter, text());
    }

private:
    void advance()
    {
        if (!isEnabled()) {  // disabled mid-hold (host state flipped): abort the fill
            cancel();
            return;
        }
        progress_ += 16.0 / kHoldMs;
        if (progress_ >= 1.0) {
            progress_ = 1.0;
            holding_ = false;
            timer_->stop();
            update();  // show the completed fill before firing
            if (on_complete_) {
                on_complete_();
            }
            progress_ = 0.0;  // reset for the next time the popup opens
            return;
        }
        update();
    }

    void cancel()
    {
        if (!holding_ && progress_ == 0.0) {
            return;
        }
        holding_ = false;
        timer_->stop();
        progress_ = 0.0;
        update();
    }

    QColor accent_;
    QColor base_ = QColor(0x3A, 0x3A, 0x3A);
    QColor text_color_ = QColor(0xF0, 0xF0, 0xF0);
    std::function<void()> on_complete_;
    QTimer* timer_ = nullptr;
    bool holding_ = false;
    double progress_ = 0.0;
};

// Hand-drawn IEC power glyph (a broken ring with a vertical stroke through the gap).
// TODO(nerdfont): replace with nf-fa-power_off once a nerdfont is bundled.
void draw_power_glyph(QPainter& painter, const QRectF& bounds, const QColor& color)
{
    const QPointF center = bounds.center();
    const double radius = std::min(bounds.width(), bounds.height()) / 2.0 - 1.0;
    QPen pen(color, 1.6);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const QRectF arc(center.x() - radius, center.y() - radius, 2 * radius, 2 * radius);
    painter.drawArc(arc, 120 * 16, 300 * 16);  // gap centred on top
    painter.drawLine(QPointF(center.x(), center.y() - radius - 1.0),
                     QPointF(center.x(), center.y() - 1.0));
}

} // namespace

// The popup panel of power actions -- all four are hold-to-fire HoldButtons (no
// instant clicks). Built once per controller (caps fixed). update_state() (called on
// open and on a live state change while open) refreshes the status line and
// enables/disables actions: On is disabled when the host is On; Off/Reset are disabled
// when it is Off; Unknown enables everything (ATEN has no status; also covers a host
// whose power state can't be read). Qt::Popup auto-dismisses on an outside click.
class PowerPopup : public QFrame {
public:
    PowerPopup(PowerCapabilities caps, std::function<void(PowerAction)> issue, QWidget* parent)
        : QFrame(parent, Qt::Popup)
        , issue_(std::move(issue))
        , query_status_(caps.query_status)
    {
        setObjectName(QStringLiteral("PowerPopup"));
        layout_ = new QVBoxLayout(this);
        layout_->setContentsMargins(12, 12, 12, 12);
        layout_->setSpacing(6);

        status_ = new QLabel(this);
        status_->setVisible(query_status_);
        layout_->addWidget(status_);

        if (caps.on) {
            add_action(QStringLiteral("Power On"), QColor(0x3F, 0xB9, 0x50), PowerAction::On);
        }
        if (caps.off_graceful) {
            add_action(QStringLiteral("Graceful Shutdown"), QColor(0xC9, 0x8A, 0x2B), PowerAction::OffGraceful);
        }
        if (caps.off_hard) {
            add_action(QStringLiteral("Force Off"), QColor(0xC4, 0x3A, 0x31), PowerAction::OffHard);
        }
        if (caps.reset) {
            add_action(QStringLiteral("Reset"), QColor(0x3B, 0x82, 0xC4), PowerAction::Reset);
        }
        setMinimumWidth(232);
    }

    void update_state(PowerState state)
    {
        if (query_status_) {
            QString color;
            QString label;
            switch (state) {
            case PowerState::On:      color = QStringLiteral("#3FB950"); label = QStringLiteral("On"); break;
            case PowerState::Off:     color = QStringLiteral("#C43A31"); label = QStringLiteral("Off"); break;
            case PowerState::Unknown: color = QStringLiteral("#888888"); label = QStringLiteral("Unknown"); break;
            }
            status_->setText(
                QStringLiteral("<span style='color:%1;'>&#9679;</span>&nbsp; Host power: %2").arg(color, label));
        }
        for (const auto& entry : actions_) {
            entry.second->setEnabled(action_enabled(entry.first, state));
        }
    }

    void apply_theme(bool dark)
    {
        const QString bg = dark ? QStringLiteral("#2B2B2B") : QStringLiteral("#FAFAFA");
        const QString border = dark ? QStringLiteral("#454545") : QStringLiteral("#C8C8C8");
        const QString fg = dark ? QStringLiteral("#F0F0F0") : QStringLiteral("#202020");
        setStyleSheet(QStringLiteral(
                          "QFrame#PowerPopup { background:%1; border:1px solid %2; border-radius:8px; }"
                          "QLabel { color:%3; background:transparent; }")
                          .arg(bg, border, fg));
        for (const auto& entry : actions_) {
            entry.second->set_theme(dark);
        }
    }

private:
    // On is pointless once On; Off/Reset are pointless (and no-op) once Off. Unknown
    // gates nothing -- there's no reliable state, so allow every action.
    static bool action_enabled(PowerAction action, PowerState state)
    {
        if (state == PowerState::Unknown) {
            return true;
        }
        switch (action) {
        case PowerAction::On:
            return state != PowerState::On;
        case PowerAction::OffGraceful:
        case PowerAction::OffHard:
        case PowerAction::Reset:
            return state != PowerState::Off;
        }
        return true;
    }

    void add_action(const QString& label, QColor accent, PowerAction action)
    {
        auto* hold = new HoldButton(label, accent, [this, action] { fire(action); }, this);
        actions_.emplace_back(action, hold);
        layout_->addWidget(hold);
    }

    void fire(PowerAction action)
    {
        if (issue_) {
            issue_(action);
        }
        close();
    }

    std::function<void(PowerAction)> issue_;
    bool query_status_ = false;
    QVBoxLayout* layout_ = nullptr;
    QLabel* status_ = nullptr;
    std::vector<std::pair<PowerAction, HoldButton*>> actions_;
};

ViewerPowerControl::ViewerPowerControl(QWidget* parent)
    : QAbstractButton(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setToolTip(QStringLiteral("Power"));
    setVisible(false);  // shown once a controller is bound

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(400);
    connect(poll_timer_, &QTimer::timeout, this, &ViewerPowerControl::poll);
    poll_timer_->start();

    pulse_timer_ = new QTimer(this);
    pulse_timer_->setInterval(33);
    connect(pulse_timer_, &QTimer::timeout, this, [this] {
        pulse_phase_ += 0.18;
        update();
    });

    connect(this, &QAbstractButton::clicked, this, &ViewerPowerControl::open_popup);
}

QSize ViewerPowerControl::sizeHint() const
{
    return QSize(kButtonWidth, kButtonHeight);
}

void ViewerPowerControl::enterEvent(QEnterEvent* event)
{
    hovered_ = true;
    update();
    QAbstractButton::enterEvent(event);
}

void ViewerPowerControl::leaveEvent(QEvent* event)
{
    // Fires when the cursor exits into the draggable caption or off-window (a real
    // client/non-client crossing). The exit into the video surface produces no leave --
    // clear_hover() handles that, driven by the surface's enter.
    hovered_ = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void ViewerPowerControl::clear_hover()
{
    if (hovered_) {
        hovered_ = false;
        update();
    }
}

void ViewerPowerControl::set_toast_manager(ToastManager* toasts)
{
    toasts_ = toasts;
}

void ViewerPowerControl::apply_theme(bool dark)
{
    dark_ = dark;
    fg_neutral_ = dark ? QColor(0xF0, 0xF0, 0xF0) : QColor(0x1A, 0x1A, 0x1A);
    hover_bg_ = dark ? QColor(255, 255, 255, 25) : QColor(0, 0, 0, 20);
    if (popup_ != nullptr) {
        popup_->apply_theme(dark);
    }
    update();
}

void ViewerPowerControl::set_controller(PowerController* controller)
{
    controller_ = controller;
    set_pending(false);
    state_ = PowerState::Unknown;
    if (popup_ != nullptr) {
        popup_->deleteLater();
        popup_ = nullptr;
    }
    if (controller_ != nullptr) {
        caps_ = controller_->capabilities();
        setVisible(caps_.any_action());
    } else {
        caps_ = PowerCapabilities{};
        setVisible(false);
    }
    update();
}

void ViewerPowerControl::open_popup()
{
    if (controller_ == nullptr) {
        return;
    }
    if (popup_ == nullptr) {
        popup_ = new PowerPopup(caps_, [this](PowerAction action) { issue(action); }, this);
        popup_->apply_theme(dark_);
    }
    popup_->update_state(state_);
    popup_->adjustSize();
    // Drop down from the button's left edge, extending right: the button lives at the left
    // of the title bar, so the window (and the screen space) is to the right. The old
    // right-aligned anchor ran the panel off the left edge when maximized.
    popup_->move(mapToGlobal(QPoint(0, height() + 2)));
    popup_->show();
}

void ViewerPowerControl::issue(PowerAction action)
{
    if (controller_ == nullptr) {
        return;
    }
    controller_->request(action);
    pending_ticks_ = 20;  // ~8s at the 400ms poll: clears the pulse if no outcome lands
    set_pending(true);
}

void ViewerPowerControl::set_pending(bool pending)
{
    pending_ = pending;
    if (pending_) {
        if (!pulse_timer_->isActive()) {
            pulse_timer_->start();
        }
    } else {
        pulse_timer_->stop();
        pulse_phase_ = 0.0;
    }
    update();
}

void ViewerPowerControl::poll()
{
    if (controller_ == nullptr) {
        return;
    }

    const PowerState state = controller_->state();
    if (state != state_) {
        state_ = state;
        if (popup_ != nullptr && popup_->isVisible()) {
            popup_->update_state(state_);
        }
        update();
    }

    bool had_outcome = false;
    for (const PowerOutcome& outcome : controller_->take_outcomes()) {
        had_outcome = true;
        if (toasts_ == nullptr) {
            continue;
        }
        if (outcome.ok) {
            toasts_->show(QString::fromUtf8(power_action_label(outcome.action)), ToastManager::Level::Success);
        } else {
            const QString detail = outcome.detail.empty()
                ? QStringLiteral("failed")
                : QString::fromStdString(outcome.detail);
            toasts_->show(QStringLiteral("Power: ") + detail, ToastManager::Level::Error, 4200);
        }
    }

    if (had_outcome) {
        set_pending(false);
    } else if (pending_ && --pending_ticks_ <= 0) {
        set_pending(false);
    }
}

void ViewerPowerControl::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (hovered_ || isDown()) {
        painter.fillRect(rect(), hover_bg_);
    }

    QColor color = fg_neutral_;
    if (caps_.query_status) {
        switch (state_) {
        case PowerState::On:      color = QColor(0x3F, 0xB9, 0x50); break;
        case PowerState::Off:     color = QColor(0xC4, 0x3A, 0x31); break;
        case PowerState::Unknown: color = fg_neutral_; break;
        }
    }
    if (pending_) {
        const double level = 0.4 + 0.6 * (0.5 + 0.5 * std::sin(pulse_phase_));
        color.setAlphaF(static_cast<float>(level));
    }

    const double box = 16.0;
    const QPointF center(width() / 2.0, height() / 2.0);
    draw_power_glyph(painter, QRectF(center.x() - box / 2, center.y() - box / 2, box, box), color);
}

} // namespace hitsc
