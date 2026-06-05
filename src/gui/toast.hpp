#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include <vector>

class QWidget;

namespace hitsc {

// A lightweight, reusable transient-notification ("toast") system. Messages appear as
// small translucent panels stacked at the bottom-centre of an anchor window, fade in,
// linger, then fade out and self-delete. Each toast is its own top-level window (not a
// child widget) so it renders correctly over a native D3D11/RHI surface. Not specific
// to power control -- repurpose for clipboard / virtual-media / etc.
class ToastManager : public QObject {
    Q_OBJECT

public:
    enum class Level { Info, Success, Error };

    explicit ToastManager(QWidget* anchor, QObject* parent = nullptr);

    void show(const QString& text, Level level = Level::Info, int duration_ms = 2600);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void reflow();

    QPointer<QWidget> anchor_;
    std::vector<QPointer<QWidget>> toasts_;
};

} // namespace hitsc
