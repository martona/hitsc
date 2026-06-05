#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QPaintEvent;
class QResizeEvent;

namespace hitsc {

// The viewer's Qt-drawn caption (the window is frameless via QWindowKit). Holds the
// app icon, an eliding title label (the draggable region), an empty action area for
// the future clipboard / power / virtual-media buttons, and Win11-style
// minimize / maximize-restore / close buttons. The buttons are exposed so
// ViewerWindow can register them with the QWindowKit agent (snap-layouts on the
// maximize button, correct hit-testing); ViewerWindow also performs the actual
// window actions via the *Requested signals.
class ViewerTitleBar : public QWidget {
    Q_OBJECT

public:
    explicit ViewerTitleBar(QWidget* parent = nullptr);

    void set_title(const QString& text);
    void set_maximized(bool maximized);  // swap the maximize/restore glyph
    void apply_theme(bool dark);

    // System-button widgets, for QWindowKit::WidgetWindowAgent::setSystemButton.
    QWidget* icon_button() const;
    QWidget* min_button() const;
    QWidget* max_button() const;
    QWidget* close_button() const;

    // Right-aligned slot for future caption controls. Add buttons here and call
    // ViewerWindow's agent setHitTestVisible() on them so they receive clicks.
    QHBoxLayout* action_area() const { return action_area_; }

signals:
    void minimizeRequested();
    void maximizeRestoreRequested();
    void closeRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void update_elided_title();

    QLabel* icon_ = nullptr;
    QLabel* title_ = nullptr;
    QHBoxLayout* action_area_ = nullptr;
    class CaptionButton* min_ = nullptr;
    class CaptionButton* max_ = nullptr;
    class CaptionButton* close_ = nullptr;

    QString full_title_;
    QColor bar_bg_;
    QColor separator_;
    bool maximized_ = false;
};

} // namespace hitsc
