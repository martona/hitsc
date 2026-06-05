#pragma once

#include "power_control.hpp"

#include <QAbstractButton>
#include <QColor>
#include <QSize>

class QPaintEvent;
class QTimer;

namespace hitsc {

class ToastManager;
class PowerPopup;

// The title-bar power control: a caption button (a power glyph tinted by host power
// state) that opens a popup panel of actions. Power On is an instant click; Graceful /
// Force-off / Reset are hold-to-fire (no message boxes). The button pulses while an
// action is in flight; the outcome is surfaced as a toast. Talks only to a
// PowerController (set by the viewer host once a view attaches); hidden when none.
//
// TODO(nerdfont): swap the hand-drawn glyph for nf-fa-power_off (on/off) once a
// nerdfont is bundled; the popup's reset action should use nf-fa-undo.
class ViewerPowerControl : public QAbstractButton {
    Q_OBJECT

public:
    explicit ViewerPowerControl(QWidget* parent = nullptr);

    // Bind to the attached view's controller (nullptr => hide the control).
    void set_controller(PowerController* controller);
    void set_toast_manager(ToastManager* toasts);
    void apply_theme(bool dark);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void open_popup();
    void poll();
    void issue(PowerAction action);
    void set_pending(bool pending);

    PowerController* controller_ = nullptr;
    ToastManager* toasts_ = nullptr;
    PowerPopup* popup_ = nullptr;
    QTimer* poll_timer_ = nullptr;
    QTimer* pulse_timer_ = nullptr;

    PowerCapabilities caps_;
    PowerState state_ = PowerState::Unknown;
    bool pending_ = false;
    int pending_ticks_ = 0;
    double pulse_phase_ = 0.0;

    bool dark_ = true;
    QColor fg_neutral_ = QColor(0xF0, 0xF0, 0xF0);
    QColor hover_bg_ = QColor(255, 255, 255, 25);
};

} // namespace hitsc
