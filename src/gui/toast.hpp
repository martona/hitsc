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

    // Handle to a sticky toast (one that stays until updated/dismissed). Safe to keep and
    // copy; operations are no-ops once the toast is gone.
    class Handle {
    public:
        Handle() = default;
        bool valid() const { return !target_.isNull(); }

    private:
        friend class ToastManager;
        explicit Handle(QObject* target)
            : target_(target)
        {
        }
        QPointer<QObject> target_;
    };

    // A toast that does not auto-dismiss -- e.g. a live progress line. update() changes its
    // text in place; dismiss() fades it out.
    Handle show_sticky(const QString& text, Level level = Level::Info);
    void update(const Handle& handle, const QString& text);
    void dismiss(const Handle& handle);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* create_toast(const QString& text, Level level, int duration_ms);
    void reflow();

    QPointer<QWidget> anchor_;
    std::vector<QPointer<QWidget>> toasts_;
};

} // namespace hitsc
