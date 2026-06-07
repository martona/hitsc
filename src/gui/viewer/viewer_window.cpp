#include "gui/viewer/viewer_window.hpp"

#include "gui/launcher_theme.hpp"
#include "gui/toast.hpp"
#include "gui/viewer/viewer_paste_control.hpp"
#include "gui/viewer/viewer_power_control.hpp"
#include "gui/viewer/viewer_surface.hpp"
#include "gui/viewer/viewer_title_bar.hpp"
#include "gui/viewer/win_scancode.hpp"

#include <QWKWidgets/widgetwindowagent.h>

#include <QAbstractNativeEventFilter>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QStyleHints>
#include <QTimer>
#include <QVariant>
#include <QWindowStateChangeEvent>

#ifdef _WIN32
#include <functional>
#include <windows.h>
#endif

namespace hitsc {
namespace {

constexpr int kFrameIntervalMs = 16;  // ~60 Hz repaint cadence

#ifdef _WIN32

// kvm_scancode_from_windows() lives in win_scancode.{hpp,cpp} now -- it's shared with
// the clipboard-typing engine (keyboard_layout_win), which gets its scan codes from
// MapVirtualKeyEx and maps them through the same table.

// Catches key messages at the application level. Qt routes the keyboard through
// the top-level window and turns it into QKeyEvents, so the surface's own
// nativeEvent never sees WM_KEY*; an app-wide filter does, regardless of which
// widget holds focus. We read the scancode + extended bit straight from the
// message (lossless), hand it to the sink, and consume it so it reaches the guest
// rather than triggering Qt shortcuts -- but only while our window is active, so
// other windows in the process (e.g. a cert prompt dialog) still get their keys.
class KeyEventFilter : public QAbstractNativeEventFilter {
public:
    KeyEventFilter(const QWidget& window, std::function<void(const KvmKeyEvent&)> sink)
        : window_(window)
        , sink_(std::move(sink))
    {
    }

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override
    {
        (void)eventType;
        const MSG* msg = static_cast<const MSG*>(message);
        switch (msg->message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            if (!window_.isActiveWindow()) {
                return false;  // not our window -- let Qt / other windows handle it
            }
            const bool down = msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN;
            // Bit 30 of lParam is the previous key state; a held key repeats. The
            // guest synthesizes its own auto-repeat, so we forward edges only.
            const bool repeat = down && (msg->lParam & (1ll << 30)) != 0;
            if (!repeat) {
                const unsigned scancode = static_cast<unsigned>((msg->lParam >> 16) & 0xFF);
                const bool extended = (msg->lParam & (1ll << 24)) != 0;
                const KvmScancode scan = kvm_scancode_from_windows(scancode, extended);
                if (scan != KvmScancode::UNKNOWN) {
                    sink_(KvmKeyEvent{scan, down, false});
                }
            }
            if (result != nullptr) {
                *result = 0;
            }
            return true;  // the keystroke belongs to the guest
        }
        default:
            break;
        }
        return false;
    }

private:
    const QWidget& window_;
    std::function<void(const KvmKeyEvent&)> sink_;
};

#endif // _WIN32

} // namespace

ViewerWindow::ViewerWindow(const QString& title, QWidget* parent)
    : QMainWindow(parent)
{
    setWindowIcon(QIcon(QStringLiteral(":/icons/hitsc-256.png")));
    resize(1280, 720);  // sane default; saved placement is wired in the entry point

    // Take the non-client frame from the OS and draw our own caption. QWindowKit
    // keeps the native behaviors (snap, drop shadow, resize borders, Win11
    // snap-layouts, system menu); we only supply the title-bar content.
    window_agent_ = new QWK::WidgetWindowAgent(this);
    window_agent_->setup(this);

    title_bar_ = new ViewerTitleBar(this);
    setMenuWidget(title_bar_);  // QMainWindow menu slot: full width, above the surface

    window_agent_->setTitleBar(title_bar_);
    window_agent_->setSystemButton(QWK::WindowAgentBase::WindowIcon, title_bar_->icon_button());
    window_agent_->setSystemButton(QWK::WindowAgentBase::Minimize, title_bar_->min_button());
    window_agent_->setSystemButton(QWK::WindowAgentBase::Maximize, title_bar_->max_button());
    window_agent_->setSystemButton(QWK::WindowAgentBase::Close, title_bar_->close_button());

    connect(title_bar_, &ViewerTitleBar::minimizeRequested, this, &QWidget::showMinimized);
    connect(title_bar_, &ViewerTitleBar::maximizeRestoreRequested, this, [this] {
        if (isMaximized()) {
            showNormal();
        } else {
            showMaximized();
        }
    });
    connect(title_bar_, &ViewerTitleBar::closeRequested, this, &QWidget::close);

    // Title-bar power control + a window-level toast host (reusable for future caption
    // features). The control stays hidden until the viewer host binds a controller.
    toast_manager_ = new ToastManager(this, this);
    power_control_ = new ViewerPowerControl(this);
    power_control_->set_toast_manager(toast_manager_);
    title_bar_->left_action_area()->addWidget(power_control_);
    window_agent_->setHitTestVisible(power_control_, true);

    paste_control_ = new ViewerPasteControl(this);
    paste_control_->set_toast_manager(toast_manager_);
    title_bar_->left_action_area()->addWidget(paste_control_);
    window_agent_->setHitTestVisible(paste_control_, true);

    set_title(title);

    apply_caption_theme();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this](Qt::ColorScheme) { apply_caption_theme(); });

    surface_ = new ViewerSurface(this);
    setCentralWidget(surface_);
    surface_->setFocus();

    // power_control_ is hit-test-visible, so QWindowKit reports it as HTCLIENT: moving the
    // cursor off it into the surface crosses no client/non-client boundary, Windows posts
    // no WM_MOUSELEAVE, and Qt delivers no leaveEvent -- its hover would stick. The surface
    // (a plain composited widget) gets a reliable enter; use it to drop the highlight.
    connect(surface_, &ViewerSurface::cursorEntered, power_control_,
            &ViewerPowerControl::clear_hover);
    connect(surface_, &ViewerSurface::cursorEntered, paste_control_,
            &ViewerPasteControl::clear_hover);

    frame_timer_ = new QTimer(this);
    frame_timer_->setInterval(kFrameIntervalMs);
    connect(frame_timer_, &QTimer::timeout, this, &ViewerWindow::frameTick);
    frame_timer_->start();

#ifdef _WIN32
    key_filter_ = std::make_unique<KeyEventFilter>(
        *this, [this](const KvmKeyEvent& key) { emit keyEvent(key); });
    QCoreApplication::instance()->installNativeEventFilter(key_filter_.get());
#endif
}

ViewerWindow::~ViewerWindow()
{
    if (key_filter_) {
        if (auto* app = QCoreApplication::instance()) {
            app->removeNativeEventFilter(key_filter_.get());
        }
    }
}

void ViewerWindow::set_title(const QString& title)
{
    setWindowTitle(title);  // taskbar / Alt-Tab
    if (title_bar_ != nullptr) {
        title_bar_->set_title(title);
    }
}

void ViewerWindow::set_power_controller(PowerController* controller)
{
    if (power_control_ != nullptr) {
        power_control_->set_controller(controller);
    }
}

void ViewerWindow::set_session_connected(bool connected)
{
    if (paste_control_ != nullptr) {
        paste_control_->set_paste_enabled(connected);
    }
    if (power_control_ != nullptr) {
        power_control_->setEnabled(connected);
    }
}

void ViewerWindow::apply_caption_theme()
{
    const bool dark = launcher_should_use_dark_theme(QGuiApplication::styleHints()->colorScheme());
    if (title_bar_ != nullptr) {
        title_bar_->apply_theme(dark);
    }
    if (power_control_ != nullptr) {
        power_control_->apply_theme(dark);
    }
    if (paste_control_ != nullptr) {
        paste_control_->apply_theme(dark);
    }
    if (window_agent_ != nullptr) {
        // Dark-mode the 1px system border QWindowKit keeps + the DWM bits.
        window_agent_->setWindowAttribute(QStringLiteral("dark-mode"), dark);
    }
}

void ViewerWindow::closeEvent(QCloseEvent* event)
{
    emit closeRequested();
    event->accept();
}

void ViewerWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::WindowStateChange) {
        const auto* state_event = static_cast<QWindowStateChangeEvent*>(event);
        const bool was_minimized = state_event->oldState().testFlag(Qt::WindowMinimized);
        const bool is_minimized = windowState().testFlag(Qt::WindowMinimized);
        if (is_minimized && !was_minimized) {
            emit minimized();
        } else if (!is_minimized && was_minimized) {
            emit restored();
        }
        if (title_bar_ != nullptr) {
            title_bar_->set_maximized(isMaximized());
        }
    }
    QMainWindow::changeEvent(event);
}

} // namespace hitsc
