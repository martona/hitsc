#pragma once

#include "console_screen.hpp"

#include <exception>
#include <functional>
#include <memory>
#include <string>

namespace hitsc {

class KvmViewBase;
class ViewerWindow;

// Identifies the host a viewer window is for.
struct ViewerLaunch {
    std::string host_label;  // window title + the "Connecting to <host>" console
    std::string host_id;     // per-host geometry key (HostStore); empty = no persistence
};

// Handle passed to run_viewer's on_ready callback. Lets a mode attach its view
// (immediately, or later after async work like auto-detection) and report a
// fatal error. Owned by the harness; valid for the window's lifetime.
class ViewerHost {
public:
    virtual ~ViewerHost() = default;

    // The viewer window, for modes that need to hook its signals (auto polls
    // frameTick for detection completion).
    virtual ViewerWindow& window() = 0;

    // Hand the harness the view to drive: takes ownership, binds the RHI D3D11
    // device, and starts the network. Call once.
    virtual void attach_view(std::unique_ptr<KvmViewBase> view) = 0;

    // Report a fatal error (e.g. auto-detection failure): closes the window;
    // run_viewer rethrows it after the event loop exits.
    virtual void fail(std::exception_ptr error) = 0;

    // Replace the default "Connecting to <host>" console shown while no view is
    // attached. For view-less flows (BMC cold reset) that end on a status screen
    // instead of a KVM session. GUI thread only. No effect once a view attaches.
    virtual void set_console(const ConsoleScreen& screen) = 0;
};

// Run a viewer window Qt-natively. Owns the QApplication (creating one if none
// exists), the ViewerWindow + surface, geometry persistence, cert-prompt
// parenting, signal wiring, app.exec(), and teardown. While no view is attached
// it shows a "Connecting to <host>" console (Esc closes). `on_ready` runs once,
// on the GUI thread, when the surface's RHI device is up; it attaches a view now
// (pikvm/aten/megarac) or arranges to attach one after detection (auto). Returns
// the QApplication exit code; rethrows any error set via ViewerHost::fail.
int run_viewer(const ViewerLaunch& launch, const std::function<void(ViewerHost&)>& on_ready);

} // namespace hitsc
