#include "view_console.hpp"

#include "log.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace hitsc {
namespace {

namespace trivial = boost::log::trivial;

constexpr float kGlyphWidth = 8.0f;
constexpr float kGlyphHeight = 8.0f;
constexpr float kLineHeight = 11.0f;
constexpr float kMargin = 16.0f;
constexpr float kGlyphAspect = 0.75f; // render the 8x8 glyphs at a 6:8 (x:y) ratio

void set_color(SDL_Renderer* renderer, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
}

void set_log_color(SDL_Renderer* renderer, trivial::severity_level severity)
{
    switch (severity) {
    case trivial::fatal:
    case trivial::error:
        set_color(renderer, 224, 108, 108);
        break;
    case trivial::warning:
        set_color(renderer, 214, 178, 96);
        break;
    default:
        set_color(renderer, 138, 146, 156);
        break;
    }
}

// 8px debug glyphs are tiny on modern displays; scale up (and track DPI) so the
// console is readable rather than squint-inducing.
float text_scale(SDL_Window* window)
{
    float display_scale = SDL_GetWindowDisplayScale(window);
    if (display_scale <= 0.0f) {
        display_scale = 1.0f;
    }
    return std::max(1.33f, display_scale * 1.33f);
}

std::string clamp_width(const std::string& text, int max_chars)
{
    if (max_chars > 0 && static_cast<int>(text.size()) > max_chars) {
        return text.substr(0, static_cast<std::size_t>(max_chars));
    }
    return text;
}

std::vector<std::string> wrap_text(const std::string& text, int max_chars)
{
    std::vector<std::string> lines;
    if (max_chars <= 0 || text.empty()) {
        lines.push_back(text);
        return lines;
    }
    for (std::size_t pos = 0; pos < text.size(); pos += static_cast<std::size_t>(max_chars)) {
        lines.push_back(text.substr(pos, static_cast<std::size_t>(max_chars)));
    }
    return lines;
}

} // namespace

void render_view_console(SDL_Renderer* renderer, SDL_Window* window, const ConsoleScreen& screen)
{
    set_color(renderer, 12, 14, 18);
    SDL_RenderClear(renderer);

    const float scale_y = text_scale(window);
    const float scale_x = scale_y * kGlyphAspect;
    SDL_SetRenderScale(renderer, scale_x, scale_y);

    int output_width = 0;
    int output_height = 0;
    SDL_GetRenderOutputSize(renderer, &output_width, &output_height);
    const float logical_width = static_cast<float>(output_width) / scale_x;
    const float logical_height = static_cast<float>(output_height) / scale_y;
    const int max_chars = static_cast<int>((logical_width - kMargin * 2.0f) / kGlyphWidth);

    float y = kMargin;

    if (!screen.headline.empty()) {
        if (screen.severity == ConsoleSeverity::Error) {
            set_color(renderer, 232, 96, 96);
        } else {
            set_color(renderer, 226, 230, 235);
        }
        SDL_RenderDebugText(renderer, kMargin, y, clamp_width(screen.headline, max_chars).c_str());
        y += kLineHeight;
    }

    if (!screen.detail.empty()) {
        set_color(renderer, 178, 184, 192);
        for (const std::string& line : wrap_text(screen.detail, max_chars)) {
            SDL_RenderDebugText(renderer, kMargin, y, line.c_str());
            y += kLineHeight;
        }
    }

    y += kLineHeight; // gap before the log tail

    const float hint_y = logical_height - kMargin - kGlyphHeight;
    const float logs_top = y;
    const float logs_bottom = screen.hint.empty() ? (logical_height - kMargin) : (hint_y - kLineHeight);
    const int max_log_lines = static_cast<int>((logs_bottom - logs_top) / kLineHeight);

    if (max_log_lines > 0) {
        const std::vector<LogEntry> entries = recent_log_lines(static_cast<std::size_t>(max_log_lines));
        std::vector<std::pair<boost::log::trivial::severity_level, std::string>> rows;
        for (const LogEntry& entry : entries) {
            for (std::string& line : wrap_text(entry.text, max_chars)) {
                rows.emplace_back(entry.severity, std::move(line));
            }
        }

        // Keep the most recent rows that fit.
        const std::size_t max_rows = static_cast<std::size_t>(max_log_lines);
        const std::size_t first = rows.size() > max_rows ? rows.size() - max_rows : 0;
        float log_y = logs_top;
        for (std::size_t i = first; i < rows.size(); ++i) {
            set_log_color(renderer, rows[i].first);
            SDL_RenderDebugText(renderer, kMargin, log_y, rows[i].second.c_str());
            log_y += kLineHeight;
        }
    }

    if (!screen.hint.empty()) {
        set_color(renderer, 120, 128, 138);
        SDL_RenderDebugText(renderer, kMargin, hint_y, clamp_width(screen.hint, max_chars).c_str());
    }

    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
    SDL_RenderPresent(renderer);
}

} // namespace hitsc
