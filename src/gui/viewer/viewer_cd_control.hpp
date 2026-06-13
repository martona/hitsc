#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QSize>

class QEnterEvent;
class QEvent;
class QPaintEvent;

namespace hitsc {

// Title-bar "mount virtual media" control: a caption button (an optical-disc glyph) that
// mounts a client-side .iso onto the host as a redirected CD over the BMC's virtual-media
// channel. The window shows it only for backends that advertise virtual-media support
// (currently MegaRAC, via KvmViewBase::hosted_supports_virtual_media()) and enables the
// action only while a session is connected.
//
// This is the UI nudge point. Clicking emits mountRequested(); the actual IUSB /cd-server
// transport + ISO block source (see the virtual-media design notes) is wired in later.
class ViewerCdControl : public QAbstractButton {
    Q_OBJECT

public:
    explicit ViewerCdControl(QWidget* parent = nullptr);

    void apply_theme(bool dark);

    // Enable/disable the mount action (the glyph dims while disabled). Disabled while
    // there's no connected guest to mount into. Separate from the control's *visibility*,
    // which the window drives from the backend's virtual-media capability.
    void set_mount_enabled(bool enabled);

    // Drop the hover highlight. As a hit-test-visible caption widget this button gets no
    // leaveEvent when the cursor exits into the surface (QWindowKit reports it HTCLIENT,
    // so Windows posts no WM_MOUSELEAVE); the window clears it on surface-enter -- the same
    // quirk the paste and power controls work around.
    void clear_hover();

    QSize sizeHint() const override;

signals:
    void mountRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    bool dark_ = true;
    bool hovered_ = false;
    bool mount_enabled_ = true;
    QColor fg_neutral_ = QColor(0xF0, 0xF0, 0xF0);
    QColor hover_bg_ = QColor(255, 255, 255, 25);
};

} // namespace hitsc
