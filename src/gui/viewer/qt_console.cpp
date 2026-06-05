#include "gui/viewer/qt_console.hpp"

#include "console_screen.hpp"
#include "log.hpp"

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QString>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace hitsc {
namespace {

namespace trivial = boost::log::trivial;

constexpr int kMargin = 16;
constexpr int kFontPixelSize = 14;

QColor log_color(trivial::severity_level severity)
{
    switch (severity) {
    case trivial::fatal:
    case trivial::error:
        return QColor(224, 108, 108);
    case trivial::warning:
        return QColor(214, 178, 96);
    default:
        return QColor(138, 146, 156);
    }
}

QString clamp_width(const QString& text, int max_chars)
{
    return (max_chars > 0 && text.size() > max_chars) ? text.left(max_chars) : text;
}

std::vector<QString> wrap_text(const QString& text, int max_chars)
{
    std::vector<QString> lines;
    if (max_chars <= 0 || text.isEmpty()) {
        lines.push_back(text);
        return lines;
    }
    for (int pos = 0; pos < text.size(); pos += max_chars) {
        lines.push_back(text.mid(pos, max_chars));
    }
    return lines;
}

} // namespace

void render_console_qpainter(QPainter& painter, const QSize& size, const ConsoleScreen& screen)
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(kFontPixelSize);
    painter.setFont(font);

    const QFontMetrics metrics(font);
    const int line_height = metrics.lineSpacing();
    const int glyph_height = metrics.height();
    const int char_width = std::max(1, metrics.averageCharWidth());

    painter.fillRect(QRect(QPoint(0, 0), size), QColor(12, 14, 18));

    const int max_chars = (size.width() - kMargin * 2) / char_width;

    const auto draw_line = [&](const QString& text, const QColor& color, int top) {
        painter.setPen(color);
        painter.drawText(kMargin, top + metrics.ascent(), text);
    };

    // Chronological top-to-bottom: the log tail (oldest -> newest) fills from the
    // top; the status block (headline + detail) -- the CURRENT state, i.e. newest --
    // sits just below the logs; the hint is pinned to the bottom edge. (The status
    // block used to be at the TOP, which read backwards against a log tail whose
    // newest line is at the bottom.) Colour already distinguishes the three regions.

    // Build the status block: headline (bright / error-red) then its detail lines.
    std::vector<std::pair<QColor, QString>> status_rows;
    if (!screen.headline.empty()) {
        const QColor color = screen.severity == ConsoleSeverity::Error
            ? QColor(232, 96, 96)
            : QColor(226, 230, 235);
        status_rows.emplace_back(color, clamp_width(QString::fromStdString(screen.headline), max_chars));
    }
    if (!screen.detail.empty()) {
        const QColor color(178, 184, 192);
        for (const QString& line : wrap_text(QString::fromStdString(screen.detail), max_chars)) {
            status_rows.emplace_back(color, line);
        }
    }

    // Lay out from the bottom up: hint at the edge, status block above it, logs fill
    // the rest. The "- line_height" terms are one-line gaps between the regions.
    const int hint_y = size.height() - kMargin - glyph_height;
    const int status_bottom = screen.hint.empty() ? (size.height() - kMargin) : (hint_y - line_height);
    const int status_top = status_bottom - static_cast<int>(status_rows.size()) * line_height;

    const int logs_top = kMargin;
    const int logs_bottom = status_rows.empty() ? status_bottom : (status_top - line_height);
    const int max_log_lines = line_height > 0 ? std::max(0, (logs_bottom - logs_top) / line_height) : 0;

    if (max_log_lines > 0) {
        const std::vector<LogEntry> entries = recent_log_lines(static_cast<std::size_t>(max_log_lines));
        std::vector<std::pair<trivial::severity_level, QString>> rows;
        for (const LogEntry& entry : entries) {
            for (const QString& line : wrap_text(QString::fromStdString(entry.text), max_chars)) {
                rows.emplace_back(entry.severity, line);
            }
        }

        // Keep the most recent rows that fit (oldest at the top, newest at the bottom).
        const std::size_t max_rows = static_cast<std::size_t>(max_log_lines);
        const std::size_t first = rows.size() > max_rows ? rows.size() - max_rows : 0;
        int log_y = logs_top;
        for (std::size_t i = first; i < rows.size(); ++i) {
            draw_line(rows[i].second, log_color(rows[i].first), log_y);
            log_y += line_height;
        }
    }

    // Status block, just below the log tail.
    int status_y = status_top;
    for (const auto& [color, text] : status_rows) {
        draw_line(text, color, status_y);
        status_y += line_height;
    }

    if (!screen.hint.empty()) {
        draw_line(clamp_width(QString::fromStdString(screen.hint), max_chars), QColor(120, 128, 138), hint_y);
    }
}

} // namespace hitsc
