#pragma once

#include "window_prefs_store.hpp"

#include <QObject>
#include <QString>

class QWindow;

namespace hitsc {

// Owns the launcher window's geometry persistence and its expanded/mini view
// mode. Exposed to QML as a context property so the header toggle can call
// setMode(); each mode remembers its own geometry, and the mini mode is a
// fixed-size, non-resizable window.
class WindowPlacementController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode NOTIFY modeChanged)

public:
    WindowPlacementController(
        WindowPrefsStore store,
        QString preference_name,
        QObject* parent = nullptr);
    ~WindowPlacementController() override;

    // Attach the QML root window after it is created. The controller is built
    // before the window exists so it can be registered as a context property
    // ahead of engine.load().
    void attach(QWindow* window);

    // Apply the persisted view mode and that mode's saved geometry.
    void restore();

    // Persist the current mode's geometry (e.g. on application quit, which may
    // not deliver a window Close event).
    void save();

    QString mode() const;

    Q_INVOKABLE void setMode(const QString& mode);

signals:
    void modeChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    static bool is_rect_within_virtual_desktop(const QRect& rect);
    void save_if_visible();
    QString rect_key(const QString& mode) const;
    void apply_mode_geometry(const QString& mode);

    QWindow* window_ = nullptr;
    WindowPrefsStore store_;
    QString preference_name_;
    QString mode_;
};

} // namespace hitsc
