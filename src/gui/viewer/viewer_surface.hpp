#pragma once

#include "view_console.hpp"      // ConsoleScreen (NOTE: still pulls SDL transitively;
                                 // extract ConsoleScreen to an SDL-free header in cleanup)
#include "view_input_types.hpp"  // KvmPointer*, KvmMouseButton

#include <QImage>
#include <QWidget>

namespace hitsc {

// The Qt-native video surface. Paints either the current video frame (scaled and
// centered, aspect-preserved, on a dark background) or the console, and turns Qt
// mouse input into the backend-neutral Kvm pointer events (emitted as signals;
// the owning window wires them to the view's KvmInputController).
//
// Keyboard is intentionally NOT handled here: it needs the raw Windows scancode +
// extended bit (which QKeyEvent::nativeScanCode drops), so it lives in an
// app-level QAbstractNativeEventFilter installed by the window (step 3).
class ViewerSurface : public QWidget {
    Q_OBJECT

public:
    explicit ViewerSurface(QWidget* parent = nullptr);

    void show_console(const ConsoleScreen& screen);  // switch to console mode + repaint
    void show_frame(const QImage& frame);            // switch to video mode + repaint

signals:
    void pointerButton(const hitsc::KvmPointerButton& button);
    void pointerMotion(const hitsc::KvmPointerMotion& motion);
    void pointerWheel(const hitsc::KvmPointerWheel& wheel);
    void focusLost();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    bool console_active_ = true;
    ConsoleScreen console_;
    QImage frame_;
};

} // namespace hitsc
