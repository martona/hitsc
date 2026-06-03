#pragma once

#include <QPainter>
#include <QSize>

namespace hitsc {

struct ConsoleScreen;

// QPainter replacement for render_view_console()'s SDL debug-text path: a full
// surface with a dark background, a severity-styled headline and detail, a tail
// of recent log lines, and a hint pinned to the bottom. Draws into the given
// painter over a region of `size`; does not clear/flush beyond the fill.
void render_console_qpainter(QPainter& painter, const QSize& size, const ConsoleScreen& screen);

} // namespace hitsc
