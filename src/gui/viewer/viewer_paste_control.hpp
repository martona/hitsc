#pragma once

#include "gui/viewer/keyboard_layout.hpp"  // TypePlan (complete type: it's a signal param)

#include <QAbstractButton>
#include <QColor>
#include <QSize>
#include <QString>

#include <memory>

class QEnterEvent;
class QEvent;
class QMouseEvent;
class QPaintEvent;
class QTimer;

namespace hitsc {

class ToastManager;

// Title-bar "type my clipboard" control: a split button. The clipboard glyph types the
// clipboard into the guest as synthesized keystrokes; the chevron opens a menu of the
// client's installed keyboard layouts (the guest's layout must match the one chosen).
//
// Host-agnostic: it exposes the selected layout (KLID) and a layoutChanged signal; the
// viewer host glue persists it per host. Phase 1 validates and toasts only -- it builds
// the key sequence and reports its size or the first untypable character, but sends
// nothing on the wire yet.
class ViewerPasteControl : public QAbstractButton {
    Q_OBJECT

public:
    explicit ViewerPasteControl(QWidget* parent = nullptr);
    ~ViewerPasteControl() override;

    void set_toast_manager(ToastManager* toasts);
    void apply_theme(bool dark);

    // Select the layout to type with (a Windows KLID). Falls back to the client's
    // active layout if the KLID can't be loaded (e.g. uninstalled since last run).
    void set_layout(const QString& klid);
    QString current_klid() const { return klid_; }

    // Drop the hover highlight. As a hit-test-visible caption widget this button gets no
    // leaveEvent when the cursor exits into the surface (the QWindowKit quirk the power
    // control hits too), so the viewer clears it on surface-enter.
    void clear_hover();

    // Reflect the typer's state: pulse the glyph while typing, and make a click cancel.
    void set_typing(bool typing);

    QSize sizeHint() const override;

signals:
    void layoutChanged(const QString& klid);
    void pasteRequested(const TypePlan& plan);
    void cancelRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    bool in_chevron(const QPoint& pos) const;
    void open_layout_menu();
    void do_paste();

    ToastManager* toasts_ = nullptr;
    std::unique_ptr<KeyboardLayout> layout_;
    QString klid_;

    bool dark_ = true;
    bool hovered_ = false;
    bool typing_ = false;
    bool armed_ = false;  // a newline-containing paste was warned about; next click types
    double pulse_phase_ = 0.0;
    QTimer* pulse_timer_ = nullptr;
    QTimer* arm_timer_ = nullptr;
    QColor fg_neutral_ = QColor(0xF0, 0xF0, 0xF0);
    QColor hover_bg_ = QColor(255, 255, 255, 25);
};

} // namespace hitsc
