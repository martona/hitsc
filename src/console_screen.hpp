#pragma once

#include <string>

namespace hitsc {

enum class ConsoleSeverity {
    Info,
    Error,
};

// A full-screen status console (connecting / disconnected / no-signal). Rendered
// by render_console_qpainter() in the Qt viewer surface.
struct ConsoleScreen {
    ConsoleSeverity severity = ConsoleSeverity::Info;
    std::string headline;
    std::string detail;
    std::string hint;
};

} // namespace hitsc
