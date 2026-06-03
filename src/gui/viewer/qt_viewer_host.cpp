#include "gui/viewer/qt_viewer_host.hpp"

#include "cert_trust.hpp"
#include "gui/launcher_host_store.hpp"
#include "gui/viewer/viewer_surface.hpp"
#include "gui/viewer/viewer_window.hpp"
#include "view_base.hpp"
#include "view_console.hpp"
#include "view_input_types.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QRect>
#include <QString>

#include <memory>
#include <optional>
#include <stdexcept>

namespace hitsc {
namespace {

// The viewer needs a QApplication (it hosts QWidgets). Direct/child launches have
// no Qt application yet, so create one; reuse an existing QApplication if some
// caller already made it. A non-widgets QCoreApplication can't host widgets.
QApplication* ensure_application(int& argc, char** argv, std::unique_ptr<QApplication>& owned)
{
    if (QCoreApplication* existing = QCoreApplication::instance()) {
        auto* app = qobject_cast<QApplication*>(existing);
        if (app == nullptr) {
            throw std::runtime_error(
                "the Qt viewer needs a QApplication, but a non-widgets QCoreApplication exists");
        }
        return app;
    }
    owned = std::make_unique<QApplication>(argc, argv);
    return owned.get();
}

} // namespace

int run_qt_viewer(KvmViewBase& view, const std::string& host_label, const std::string& host_id)
{
    static char program_name[] = "hitsc";
    int argc = 1;
    char* argv[] = {program_name, nullptr};

    std::unique_ptr<QApplication> owned_app;
    QApplication* app = ensure_application(argc, argv, owned_app);

    ViewerWindow window(QStringLiteral("hitsc - ") + QString::fromStdString(host_label));
    ViewerSurface* surface = window.surface();

    // Restore the saved per-host geometry (the same store the SDL viewer used).
    // TODO: guard against a rect on a monitor that is no longer present (the SDL
    // path did, via rect_intersects_a_display).
    if (!host_id.empty()) {
        const HostStore store;
        if (const std::optional<QRect> rect = store.load_window_rect(QString::fromStdString(host_id))) {
            if (rect->width() > 0 && rect->height() > 0) {
                window.setGeometry(*rect);
            }
        }
    }

    // Keyboard, mirroring the SDL event_loop's gating: the disconnected console
    // takes only R (reconnect) / Esc (close); a live session forwards keys to the
    // guest; the connecting console takes Esc (cancel).
    QObject::connect(&window, &ViewerWindow::keyEvent, &window, [&view, &window](const KvmKeyEvent& key) {
        if (view.hosted_session_ended()) {
            if (key.down && key.scancode == KvmScancode::R) {
                view.hosted_retry();
            } else if (key.down && key.scancode == KvmScancode::ESCAPE) {
                window.close();
            }
            return;
        }
        if (view.hosted_connected()) {
            view.feed_key(key);
            return;
        }
        if (key.down && key.scancode == KvmScancode::ESCAPE) {
            window.close();
        }
    });

    // Pointer input only flows to a live session (matches event_loop).
    QObject::connect(surface, &ViewerSurface::pointerButton, &window, [&view](const KvmPointerButton& button) {
        if (view.hosted_connected()) {
            view.feed_pointer_button(button);
        }
    });
    QObject::connect(surface, &ViewerSurface::pointerMotion, &window, [&view](const KvmPointerMotion& motion) {
        if (view.hosted_connected()) {
            view.feed_pointer_motion(motion);
        }
    });
    QObject::connect(surface, &ViewerSurface::pointerWheel, &window, [&view](const KvmPointerWheel& wheel) {
        if (view.hosted_connected()) {
            view.feed_pointer_wheel(wheel);
        }
    });
    QObject::connect(surface, &ViewerSurface::focusLost, &window, [&view]() { view.hosted_focus_lost(); });

    QObject::connect(&window, &ViewerWindow::minimized, &window, [&view]() { view.hosted_minimized(); });
    QObject::connect(&window, &ViewerWindow::restored, &window, [&view]() { view.hosted_restored(); });

    QObject::connect(&window, &ViewerWindow::closeRequested, &window, [&view, &window, host_id]() {
        if (!host_id.empty() && !window.isMinimized() && !window.isMaximized()) {
            const HostStore store;
            store.save_window_rect(QString::fromStdString(host_id), window.geometry());
        }
        view.hosted_stop_network();
    });

    // ~16 ms cadence: advance session state, keep the surface size current for
    // pointer mapping, and push the latest console/frame to the surface.
    QObject::connect(
        &window,
        &ViewerWindow::frameTick,
        &window,
        [&view, &window, surface, last_title = QString()]() mutable {
            view.hosted_poll();
            view.hosted_set_surface_size(surface->width(), surface->height());
            if (const std::optional<ConsoleScreen> console = view.hosted_console_screen()) {
                surface->show_console(*console);
            } else if (const std::optional<QImage> frame = view.latest_frame_image()) {
                surface->show_frame(*frame);
            }
            const QString title = QString::fromStdString(view.hosted_title());
            if (title != last_title) {
                last_title = title;
                window.setWindowTitle(title);
            }
        });

    // Cert prompts parent on this window and pin under host_id (HWND on Windows).
    cert_trust_attach_window(reinterpret_cast<void*>(window.winId()), host_id);

    view.hosted_start_network();
    window.show();
    surface->setFocus();

    const int code = app->exec();

    view.hosted_stop_network();
    return code;
}

} // namespace hitsc
