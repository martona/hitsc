#pragma once

#include "virtual_media/virtual_media.hpp"  // MediaState (member by value)

#include <QAbstractButton>
#include <QColor>
#include <QSize>

class QEnterEvent;
class QEvent;
class QPaintEvent;
class QTimer;

namespace hitsc {

class ToastManager;
class VirtualMediaController;

// Title-bar "mount virtual media" control: a caption button (an optical-disc glyph) that mounts
// a client-side .iso onto the host as a redirected CD. Self-driving like ViewerPowerControl: it
// talks only to a VirtualMediaController (bound by the viewer host once a view attaches; hidden
// when none). A click opens an ISO picker when idle and ejects when mounted; the glyph reflects
// idle / mounting (pulsing) / mounted / error, and outcomes surface as toasts.
class ViewerCdControl : public QAbstractButton {
    Q_OBJECT

public:
    explicit ViewerCdControl(QWidget* parent = nullptr);

    // Bind to the attached view's controller (nullptr => hide the control).
    void set_controller(VirtualMediaController* controller);
    void set_toast_manager(ToastManager* toasts);
    void apply_theme(bool dark);

    // Drop the hover highlight. As a hit-test-visible caption widget this button gets no
    // leaveEvent when the cursor exits into the surface (QWindowKit reports it HTCLIENT); the
    // window clears it on surface-enter.
    void clear_hover();

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void poll();
    void on_clicked();
    void refresh_tooltip();
    void set_state(MediaState state);

    VirtualMediaController* controller_ = nullptr;
    ToastManager* toasts_ = nullptr;
    QTimer* poll_timer_ = nullptr;
    QTimer* pulse_timer_ = nullptr;
    MediaState state_ = MediaState::Idle;
    double pulse_phase_ = 0.0;

    bool dark_ = true;
    bool hovered_ = false;  // own hover flag: underMouse() sticks (no leaveEvent, see clear_hover)
    QColor fg_neutral_ = QColor(0xF0, 0xF0, 0xF0);
    QColor hover_bg_ = QColor(255, 255, 255, 25);
};

} // namespace hitsc
