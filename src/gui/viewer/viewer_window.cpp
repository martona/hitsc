#include "gui/viewer/viewer_window.hpp"

#include "gui/viewer/viewer_surface.hpp"

#include <QAbstractNativeEventFilter>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QTimer>
#include <QWindowStateChangeEvent>

#ifdef _WIN32
#include <functional>
#include <windows.h>
#endif

namespace hitsc {
namespace {

constexpr int kFrameIntervalMs = 16;  // ~60 Hz repaint cadence

#ifdef _WIN32

// Map a Windows PS/2 set-1 scan code (plus the extended-key flag, both read
// straight from the WM_KEY* lParam) to a KvmScancode. We read the raw lParam
// rather than QKeyEvent::nativeScanCode() because the latter drops the extended
// bit, which is what distinguishes the arrows from the numpad and the right
// modifiers from the left. (KvmScancode values are USB HID usage codes, so this
// is a direct PS/2-to-HID scancode mapping -- only the top-row digits and DELETE
// need renamed enumerators.)
KvmScancode kvm_scancode_from_windows(unsigned scancode, bool extended)
{
    if (extended) {
        switch (scancode) {
        case 0x1C: return KvmScancode::KP_ENTER;
        case 0x1D: return KvmScancode::RCTRL;
        case 0x35: return KvmScancode::KP_DIVIDE;
        case 0x37: return KvmScancode::PRINTSCREEN;
        case 0x38: return KvmScancode::RALT;
        case 0x47: return KvmScancode::HOME;
        case 0x48: return KvmScancode::UP;
        case 0x49: return KvmScancode::PAGEUP;
        case 0x4B: return KvmScancode::LEFT;
        case 0x4D: return KvmScancode::RIGHT;
        case 0x4F: return KvmScancode::END;
        case 0x50: return KvmScancode::DOWN;
        case 0x51: return KvmScancode::PAGEDOWN;
        case 0x52: return KvmScancode::INSERT;
        case 0x53: return KvmScancode::DELETE_KEY;
        case 0x5B: return KvmScancode::LGUI;
        case 0x5C: return KvmScancode::RGUI;
        case 0x5D: return KvmScancode::APPLICATION;
        default: return KvmScancode::UNKNOWN;
        }
    }

    switch (scancode) {
    case 0x01: return KvmScancode::ESCAPE;
    case 0x02: return KvmScancode::DIGIT_1;
    case 0x03: return KvmScancode::DIGIT_2;
    case 0x04: return KvmScancode::DIGIT_3;
    case 0x05: return KvmScancode::DIGIT_4;
    case 0x06: return KvmScancode::DIGIT_5;
    case 0x07: return KvmScancode::DIGIT_6;
    case 0x08: return KvmScancode::DIGIT_7;
    case 0x09: return KvmScancode::DIGIT_8;
    case 0x0A: return KvmScancode::DIGIT_9;
    case 0x0B: return KvmScancode::DIGIT_0;
    case 0x0C: return KvmScancode::MINUS;
    case 0x0D: return KvmScancode::EQUALS;
    case 0x0E: return KvmScancode::BACKSPACE;
    case 0x0F: return KvmScancode::TAB;
    case 0x10: return KvmScancode::Q;
    case 0x11: return KvmScancode::W;
    case 0x12: return KvmScancode::E;
    case 0x13: return KvmScancode::R;
    case 0x14: return KvmScancode::T;
    case 0x15: return KvmScancode::Y;
    case 0x16: return KvmScancode::U;
    case 0x17: return KvmScancode::I;
    case 0x18: return KvmScancode::O;
    case 0x19: return KvmScancode::P;
    case 0x1A: return KvmScancode::LEFTBRACKET;
    case 0x1B: return KvmScancode::RIGHTBRACKET;
    case 0x1C: return KvmScancode::RETURN;
    case 0x1D: return KvmScancode::LCTRL;
    case 0x1E: return KvmScancode::A;
    case 0x1F: return KvmScancode::S;
    case 0x20: return KvmScancode::D;
    case 0x21: return KvmScancode::F;
    case 0x22: return KvmScancode::G;
    case 0x23: return KvmScancode::H;
    case 0x24: return KvmScancode::J;
    case 0x25: return KvmScancode::K;
    case 0x26: return KvmScancode::L;
    case 0x27: return KvmScancode::SEMICOLON;
    case 0x28: return KvmScancode::APOSTROPHE;
    case 0x29: return KvmScancode::GRAVE;
    case 0x2A: return KvmScancode::LSHIFT;
    case 0x2B: return KvmScancode::BACKSLASH;
    case 0x2C: return KvmScancode::Z;
    case 0x2D: return KvmScancode::X;
    case 0x2E: return KvmScancode::C;
    case 0x2F: return KvmScancode::V;
    case 0x30: return KvmScancode::B;
    case 0x31: return KvmScancode::N;
    case 0x32: return KvmScancode::M;
    case 0x33: return KvmScancode::COMMA;
    case 0x34: return KvmScancode::PERIOD;
    case 0x35: return KvmScancode::SLASH;
    case 0x36: return KvmScancode::RSHIFT;
    case 0x37: return KvmScancode::KP_MULTIPLY;
    case 0x38: return KvmScancode::LALT;
    case 0x39: return KvmScancode::SPACE;
    case 0x3A: return KvmScancode::CAPSLOCK;
    case 0x3B: return KvmScancode::F1;
    case 0x3C: return KvmScancode::F2;
    case 0x3D: return KvmScancode::F3;
    case 0x3E: return KvmScancode::F4;
    case 0x3F: return KvmScancode::F5;
    case 0x40: return KvmScancode::F6;
    case 0x41: return KvmScancode::F7;
    case 0x42: return KvmScancode::F8;
    case 0x43: return KvmScancode::F9;
    case 0x44: return KvmScancode::F10;
    case 0x45: return KvmScancode::NUMLOCKCLEAR;
    case 0x46: return KvmScancode::SCROLLLOCK;
    case 0x47: return KvmScancode::KP_7;
    case 0x48: return KvmScancode::KP_8;
    case 0x49: return KvmScancode::KP_9;
    case 0x4A: return KvmScancode::KP_MINUS;
    case 0x4B: return KvmScancode::KP_4;
    case 0x4C: return KvmScancode::KP_5;
    case 0x4D: return KvmScancode::KP_6;
    case 0x4E: return KvmScancode::KP_PLUS;
    case 0x4F: return KvmScancode::KP_1;
    case 0x50: return KvmScancode::KP_2;
    case 0x51: return KvmScancode::KP_3;
    case 0x52: return KvmScancode::KP_0;
    case 0x53: return KvmScancode::KP_PERIOD;
    case 0x56: return KvmScancode::NONUSBACKSLASH;
    case 0x57: return KvmScancode::F11;
    case 0x58: return KvmScancode::F12;
    default: return KvmScancode::UNKNOWN;
    }
}

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
    setWindowTitle(title);
    resize(1280, 720);  // sane default; saved placement is wired in the entry point

    surface_ = new ViewerSurface(this);
    setCentralWidget(surface_);
    surface_->setFocus();

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
    }
    QMainWindow::changeEvent(event);
}

} // namespace hitsc
