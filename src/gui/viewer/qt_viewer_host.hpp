#pragma once

#include <string>

namespace hitsc {

class KvmViewBase;

// Run a view Qt-natively: create (if needed) a QApplication, build the viewer
// window, wire its input/lifecycle/frame signals to the view, start the network,
// and pump app.exec() until the window closes. This is the Qt-native replacement
// for KvmViewBase::run() (the SDL event loop) on direct and child launches; the
// SDL path stays in place for the auto-detect handoff until that goes Qt too.
// Returns the QApplication exit code.
int run_qt_viewer(KvmViewBase& view, const std::string& host_label, const std::string& host_id);

} // namespace hitsc
