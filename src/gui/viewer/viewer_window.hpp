#pragma once

#include "view_input_types.hpp"  // KvmKeyEvent

#include <QMainWindow>

#include <memory>

class QAbstractNativeEventFilter;
class QCloseEvent;
class QEvent;
class QTimer;

namespace QWK {
class WidgetWindowAgent;
}

namespace hitsc {

class PowerController;
class ToastManager;
class ViewerPowerControl;
class ViewerSurface;
class ViewerTitleBar;

// The top-level Qt-native viewer window. Owns the ViewerSurface (its central
// widget) plus the things that have to live above the surface:
//   - a ~16 ms QTimer whose frameTick() tells the driver to pull the latest
//     frame (the driver, wired in the entry point, calls surface()->show_frame /
//     show_console; step 4 also emits a frame-arrival signal for low latency);
//   - an app-wide Win32 native event filter that turns raw WM_KEY* messages into
//     KvmKeyEvents (emitted via keyEvent), because Qt converts keystrokes to
//     QKeyEvents that drop the scancode + extended bit the guest needs;
//   - window lifecycle (close / minimize / restore) surfaced as signals.
//
// Like ViewerSurface, the window is deliberately view-agnostic: it emits signals
// and exposes surface(); the entry point connects those to the KvmViewBase. This
// is what keeps the window buildable before step 4 reshapes the view.
class ViewerWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit ViewerWindow(const QString& title, QWidget* parent = nullptr);
    ~ViewerWindow() override;

    ViewerSurface* surface() const { return surface_; }

    // Set the caption text (and the OS window title used by the taskbar / Alt-Tab).
    void set_title(const QString& title);

    // Bind the title-bar power control to the attached view's controller (or null to
    // hide it). Called by the viewer host when a view attaches / detaches.
    void set_power_controller(PowerController* controller);

signals:
    void keyEvent(const hitsc::KvmKeyEvent& key);  // raw-scancode key (Win32 filter)
    void frameTick();                              // ~16 ms repaint cadence
    void closeRequested();
    void minimized();
    void restored();

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void apply_caption_theme();

    QWK::WidgetWindowAgent* window_agent_ = nullptr;
    ViewerTitleBar* title_bar_ = nullptr;
    ViewerPowerControl* power_control_ = nullptr;
    ToastManager* toast_manager_ = nullptr;
    ViewerSurface* surface_ = nullptr;
    QTimer* frame_timer_ = nullptr;
    std::unique_ptr<QAbstractNativeEventFilter> key_filter_;
};

} // namespace hitsc
