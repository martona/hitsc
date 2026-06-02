#pragma once

#include <SDL3/SDL.h>

#include <string>

namespace hitsc {

enum class ConsoleSeverity {
    Info,
    Error,
};

struct ConsoleScreen {
    ConsoleSeverity severity = ConsoleSeverity::Info;
    std::string headline;
    std::string detail;
    std::string hint;
};

// Renders a full-window console: dark background, a severity-styled headline and
// detail line, a tail of recent log lines, and a hint pinned to the bottom —
// drawn with SDL3's built-in debug text, scaled for the window's display scale.
// Clears and presents the renderer.
void render_view_console(SDL_Renderer* renderer, SDL_Window* window, const ConsoleScreen& screen);

} // namespace hitsc
