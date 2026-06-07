#include "gui/viewer/qt_viewer_host.hpp"

#include "cert_prompt.hpp"
#include "cert_trust.hpp"
#include "console_screen.hpp"
#include "gui/launcher_host_store.hpp"
#include "gui/screen_geometry.hpp"
#include "gui/toast.hpp"
#include "gui/viewer/clipboard_typer.hpp"
#include "gui/viewer/keyboard_layout.hpp"
#include "gui/viewer/main_thread_sampler.hpp"
#include "gui/viewer/viewer_paste_control.hpp"
#include "gui/viewer/viewer_surface.hpp"
#include "gui/viewer/viewer_window.hpp"
#include "view_base.hpp"
#include "view_input_types.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QMessageBox>
#include <QRect>
#include <QString>

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

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

// Concrete ViewerHost: owns the attached view (until the window closes) and
// remembers a fatal error for run_viewer to rethrow.
class ViewerHostImpl : public ViewerHost {
public:
    ViewerHostImpl(ViewerWindow& window, ViewerSurface& surface)
        : window_(window)
        , surface_(surface)
    {
    }

    ViewerWindow& window() override { return window_; }

    void attach_view(std::unique_ptr<KvmViewBase> view) override
    {
        view_ = std::move(view);
        if (view_ != nullptr) {
            view_->set_rhi_d3d11_device(surface_.d3d11_device());
            view_->hosted_start_network();
            window_.set_power_controller(view_->power_controller());
        }
    }

    void fail(std::exception_ptr error) override
    {
        error_ = std::move(error);
        window_.close();
    }

    KvmViewBase* view() const { return view_.get(); }
    std::exception_ptr error() const { return error_; }

private:
    ViewerWindow& window_;
    ViewerSurface& surface_;
    std::unique_ptr<KvmViewBase> view_;
    std::exception_ptr error_;
};

} // namespace

int run_viewer(const ViewerLaunch& launch, const std::function<void(ViewerHost&)>& on_ready)
{
    static char program_name[] = "hitsc";
    int argc = 1;
    char* argv[] = {program_name, nullptr};

    std::unique_ptr<QApplication> owned_app;
    QApplication* app = ensure_application(argc, argv, owned_app);

    ViewerWindow window(QStringLiteral("hitsc - ") + QString::fromStdString(launch.host_label));
    ViewerSurface* surface = window.surface();
    ViewerHostImpl host(window, *surface);

    const std::string host_id = launch.host_id;
    const std::string host_label = launch.host_label;

    // Restore the saved per-host geometry, ignoring a rect whose monitor is no
    // longer present (so the window never opens off-screen).
    if (!host_id.empty()) {
        const HostStore store;
        if (const std::optional<QRect> rect = store.load_window_rect(QString::fromStdString(host_id))) {
            if (rect_within_virtual_desktop(*rect)) {
                window.setGeometry(*rect);
            }
        }
    }

    // Per-host clipboard-typing layout: restore the saved KLID (default: the client's
    // active layout) and persist it whenever the user picks a different one. Lives in the
    // same per-host registry key as the window geometry above -- no IPC needed.
    ClipboardTyper clipboard_typer;
    clipboard_typer.set_hooks(
        [&host]() {
            KvmViewBase* view = host.view();
            return view != nullptr && view->hosted_connected();
        },
        [&host](const KvmKeyEvent& key) {
            KvmViewBase* view = host.view();
            if (view != nullptr && view->hosted_connected()) {
                view->feed_key(key);
            }
        });
    if (ViewerPasteControl* paste = window.paste_control()) {
        QString klid;
        if (!host_id.empty()) {
            const HostStore store;
            if (const std::optional<QString> saved =
                    store.load_keyboard_layout(QString::fromStdString(host_id))) {
                klid = *saved;
            }
        }
        if (klid.isEmpty()) {
            klid = active_keyboard_layout_klid();
        }
        paste->set_layout(klid);
        QObject::connect(
            paste, &ViewerPasteControl::layoutChanged, &window, [host_id](const QString& chosen) {
                if (!host_id.empty()) {
                    const HostStore store;
                    store.save_keyboard_layout(QString::fromStdString(host_id), chosen);
                }
            });

        // Phase 2: drive the built plan onto the wire (paced), with click-to-cancel and a
        // live progress toast that updates per keystroke and clears when done/canceled.
        auto typing_toast = std::make_shared<ToastManager::Handle>();
        QObject::connect(
            paste, &ViewerPasteControl::pasteRequested, &window,
            [&host, &window, &clipboard_typer](const TypePlan& plan) {
                KvmViewBase* view = host.view();
                if (view == nullptr || !view->hosted_connected()) {
                    window.toasts()->show(QStringLiteral("Connect to a host first"));
                    return;
                }
                clipboard_typer.start(plan);
            });
        QObject::connect(paste, &ViewerPasteControl::cancelRequested, &clipboard_typer,
                         [&clipboard_typer]() { clipboard_typer.cancel(); });
        QObject::connect(
            &clipboard_typer, &ClipboardTyper::started, paste,
            [paste, &window, typing_toast](int total) {
                paste->set_typing(true);
                *typing_toast = window.toasts()->show_sticky(
                    QStringLiteral("Typing, %1 keystrokes left").arg(total));
            });
        QObject::connect(
            &clipboard_typer, &ClipboardTyper::progress, &window,
            [&window, typing_toast](int remaining) {
                window.toasts()->update(
                    *typing_toast, QStringLiteral("Typing, %1 keystrokes left").arg(remaining));
            });
        QObject::connect(
            &clipboard_typer, &ClipboardTyper::finished, paste,
            [paste, &window, typing_toast](bool completed) {
                paste->set_typing(false);
                window.toasts()->dismiss(*typing_toast);
                if (!completed) {
                    window.toasts()->show(QStringLiteral("Canceled typing"));
                }
            });
    }

    // Keyboard gating: a live session forwards keys to the guest; the disconnected
    // console takes R (reconnect) / Esc (close); the connecting/detecting phase
    // (no view yet) takes Esc (cancel).
    QObject::connect(&window, &ViewerWindow::keyEvent, &window, [&host, &window](const KvmKeyEvent& key) {
        KvmViewBase* view = host.view();
        if (view != nullptr && view->hosted_session_ended()) {
            if (key.down && key.scancode == KvmScancode::R) {
                view->hosted_retry();
            } else if (key.down && key.scancode == KvmScancode::ESCAPE) {
                window.close();
            }
            return;
        }
        if (view != nullptr && view->hosted_connected()) {
            view->feed_key(key);
            return;
        }
        if (key.down && key.scancode == KvmScancode::ESCAPE) {
            window.close();
        }
    });

    // Pointer input only flows to a live session.
    QObject::connect(surface, &ViewerSurface::pointerButton, &window, [&host](const KvmPointerButton& button) {
        if (KvmViewBase* view = host.view(); view != nullptr && view->hosted_connected()) {
            view->feed_pointer_button(button);
        }
    });
    QObject::connect(surface, &ViewerSurface::pointerMotion, &window, [&host](const KvmPointerMotion& motion) {
        if (KvmViewBase* view = host.view(); view != nullptr && view->hosted_connected()) {
            view->feed_pointer_motion(motion);
        }
    });
    QObject::connect(surface, &ViewerSurface::pointerWheel, &window, [&host](const KvmPointerWheel& wheel) {
        if (KvmViewBase* view = host.view(); view != nullptr && view->hosted_connected()) {
            view->feed_pointer_wheel(wheel);
        }
    });
    QObject::connect(surface, &ViewerSurface::focusLost, &window, [&host]() {
        if (KvmViewBase* view = host.view()) {
            view->hosted_focus_lost();
        }
    });

    QObject::connect(&window, &ViewerWindow::minimized, &window, [&host]() {
        if (KvmViewBase* view = host.view()) {
            view->hosted_minimized();
        }
    });
    QObject::connect(&window, &ViewerWindow::restored, &window, [&host]() {
        if (KvmViewBase* view = host.view()) {
            view->hosted_restored();
        }
    });

    QObject::connect(&window, &ViewerWindow::closeRequested, &window, [&host, &window, host_id]() {
        if (!host_id.empty() && !window.isMinimized() && !window.isMaximized()) {
            const HostStore store;
            store.save_window_rect(QString::fromStdString(host_id), window.geometry());
        }
        // Release any cert prompt blocked on the GUI thread before joining the
        // network/detection thread, or the join deadlocks against it.
        cancel_pending_cert_prompt();
        if (KvmViewBase* view = host.view()) {
            view->hosted_stop_network();
        }
    });

    // ~16 ms cadence: drive the attached view (poll, size, console/frame, title),
    // or show the connecting console until a view is attached.
    QObject::connect(
        &window,
        &ViewerWindow::frameTick,
        &window,
        [&host, &window, surface, host_label, last_title = QString()]() mutable {
            KvmViewBase* view = host.view();
            if (view == nullptr) {
                ConsoleScreen screen;
                screen.headline = "Connecting to " + host_label + "...";
                screen.hint = "Esc to cancel";
                surface->show_console(screen);
                return;
            }
            view->hosted_poll();
            view->hosted_set_surface_size(surface->width(), surface->height());
            if (const std::optional<ConsoleScreen> console = view->hosted_console_screen()) {
                surface->show_console(*console);
            } else if (const std::optional<HardwareVideoFrame> hw = view->latest_hardware_frame()) {
                surface->show_hardware_frame(*hw);
            } else if (const std::optional<SoftwareFrame> frame = view->latest_frame()) {
                // base and cursor update independently: a new base re-uploads the
                // framebuffer; a cursor-only change just moves/reuploads the tiny
                // sprite quad. Either may be absent when only the other changed.
                if (frame->base) {
                    surface->show_frame(*frame->base, frame->dirty);
                }
                if (frame->cursor) {
#ifdef HITSC_DEBUG_HW_CURSOR_HIDE
                    // Overlay suppressed: if a cursor still appears on screen, it's
                    // baked into the BMC's JPEG video rather than drawn by us.
                    surface->update_cursor(QImage(), 0, 0);
#else
                    surface->update_cursor(frame->cursor->sprite, frame->cursor->x, frame->cursor->y);
#endif
                }
            }
            const QString title = QString::fromStdString(view->hosted_title());
            if (title != last_title) {
                last_title = title;
                window.set_title(title);
            }
        });

    // Cert prompts parent on this window and pin under host_id.
    cert_trust_attach_window(&window, host_id);

    // The view is attached once QRhi (and its D3D11 device) is up, so the network
    // starts only after the device exists -- pikvm binds FFmpeg D3D11VA decode to
    // the same device QRhi renders with. rhiReady fires once.
    QObject::connect(surface, &ViewerSurface::rhiReady, &window, [&host, &on_ready]() {
        // on_ready runs inside the event loop, so an exception (e.g. a view
        // constructor) must not escape into Qt; route it to fail() instead.
        try {
            on_ready(host);
        } catch (...) {
            host.fail(std::current_exception());
        }
    });

    // If QRhi can't bring up D3D11 at all there's no surface to draw on (not even
    // the console), so tell the user and close. Queued so the modal dialog runs
    // after the failed render cycle unwinds rather than inside it.
    QObject::connect(surface, &ViewerSurface::rhiUnavailable, &window, [&window]() {
        QMessageBox::critical(
            &window,
            QStringLiteral("hitsc"),
            QStringLiteral("hitsc requires Direct3D 11, which is unavailable on this system. "
                           "The viewer will now close."));
        window.close();
    }, Qt::QueuedConnection);

    window.show();
    surface->setFocus();

    // No-op unless HITSC_DEBUG_MAIN_SAMPLER is defined. Constructed on the main
    // thread so it profiles THIS thread (the Qt UI thread) across the event loop.
    MainThreadSampler main_thread_sampler;
    const int code = app->exec();

    // Unbind the power control before the view (and its controller) is destroyed.
    window.set_power_controller(nullptr);

    cancel_pending_cert_prompt();
    if (KvmViewBase* view = host.view()) {
        view->hosted_stop_network();
    }
    if (host.error()) {
        std::rethrow_exception(host.error());
    }
    return code;
}

} // namespace hitsc
