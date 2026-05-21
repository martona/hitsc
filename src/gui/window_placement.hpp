#pragma once

#include "window_prefs_store.hpp"

#include <QObject>
#include <QString>

class QWindow;

namespace hitsc {

class WindowPlacementController : public QObject {
public:
    WindowPlacementController(
        QWindow* window,
        WindowPrefsStore store,
        QString preference_name,
        QObject* parent = nullptr);
    ~WindowPlacementController() override;

    void restore();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    static bool is_rect_within_virtual_desktop(const QRect& rect);
    void save_if_visible();

    QWindow* window_ = nullptr;
    WindowPrefsStore store_;
    QString preference_name_;
};

} // namespace hitsc
