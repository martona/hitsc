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
class ViewerCdControl;
class ViewerPasteControl;
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

    // The title-bar "type clipboard" control (host glue persists its chosen layout).
    ViewerPasteControl* paste_control() const { return paste_control_; }

    // The title-bar "mount CD / ISO" control (host glue connects its mountRequested).
    ViewerCdControl* cd_control() const { return cd_control_; }

    // Set the caption text (and the OS window title used by the taskbar / Alt-Tab).
    void set_title(const QString& title);

    // Bind the title-bar power control to the attached view's controller (or null to
    // hide it). Called by the viewer host when a view attaches / detaches.
    void set_power_controller(PowerController* controller);

    // Show/hide the title-bar CD control based on whether the attached backend supports
    // virtual-media redirection. Called by the viewer host when a view attaches.
    void set_virtual_media_available(bool available);

    // The toast host (used by the glue for paste/typing feedback). Always non-null.
    ToastManager* toasts() const { return toast_manager_; }

    // Enable/disable the connection-gated caption controls (paste action, power) as the
    // session connects/disconnects. Driven from the host's frame tick.
    void set_session_connected(bool connected);

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
    ViewerPasteControl* paste_control_ = nullptr;
    ViewerCdControl* cd_control_ = nullptr;
    ToastManager* toast_manager_ = nullptr;
    ViewerSurface* surface_ = nullptr;
    QTimer* frame_timer_ = nullptr;
    std::unique_ptr<QAbstractNativeEventFilter> key_filter_;
};

} // namespace hitsc
