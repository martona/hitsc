#include "launcher_gui.hpp"

#include "launcher_host_model.hpp"
#include "launcher_theme.hpp"
#include "window_placement.hpp"
#include "window_prefs_store.hpp"

#include <QColor>
#include <QGuiApplication>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStyleHints>
#include <QUrl>
#include <QWindow>

#include <cstdlib>

#ifdef _WIN32
#include <QAbstractNativeEventFilter>

#include <dwmapi.h>
#include <windows.h>
#endif

namespace hitsc {

namespace {

QPalette make_dark_palette()
{
    QPalette palette;
    const QColor window(31, 32, 36);
    const QColor panel(42, 45, 50);
    const QColor base(24, 26, 29);
    const QColor text(241, 243, 245);
    const QColor muted_text(186, 191, 198);
    const QColor disabled_text(126, 132, 140);
    const QColor accent(76, 141, 255);

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, panel);
    palette.setColor(QPalette::ToolTipBase, panel);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, panel);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, QColor(255, 96, 96));
    palette.setColor(QPalette::Light, QColor(68, 72, 79));
    palette.setColor(QPalette::Midlight, QColor(54, 58, 64));
    palette.setColor(QPalette::Mid, QColor(48, 51, 57));
    palette.setColor(QPalette::Dark, QColor(18, 20, 23));
    palette.setColor(QPalette::Shadow, QColor(0, 0, 0));
    palette.setColor(QPalette::Highlight, accent);
    palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
    palette.setColor(QPalette::PlaceholderText, muted_text);
    palette.setColor(QPalette::Link, QColor(116, 169, 255));
    palette.setColor(QPalette::LinkVisited, QColor(177, 143, 255));

    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled_text);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabled_text);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled_text);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(56, 60, 66));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled_text);

    return palette;
}

void apply_application_theme(QGuiApplication& app, const QPalette& light_palette, Qt::ColorScheme color_scheme)
{
    const bool dark = launcher_should_use_dark_theme(color_scheme);
    app.styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    app.setPalette(dark ? make_dark_palette() : light_palette);
}

#ifdef _WIN32
COLORREF to_color_ref(const QColor& color)
{
    return RGB(color.red(), color.green(), color.blue());
}

// windows loves flashbanging you while the window is being resized
// and qt can't do its damn job, so we do it for them
class LauncherBackgroundEraseFilter : public QAbstractNativeEventFilter {
public:
    void set_window(QWindow* window)
    {
        hwnd_ = window == nullptr ? nullptr : reinterpret_cast<HWND>(window->winId());
    }

    void set_color(const QColor& color)
    {
        color_ = to_color_ref(color);
    }

    bool nativeEventFilter(
        const QByteArray& event_type,
        void* message,
        qintptr* result) override
    {
        if (hwnd_ == nullptr
            || (event_type != "windows_generic_MSG" && event_type != "windows_dispatcher_MSG")) {
            return false;
        }

        const auto* msg = static_cast<MSG*>(message);
        if (msg == nullptr || msg->hwnd != hwnd_ || msg->message != WM_ERASEBKGND) {
            return false;
        }

        const HDC hdc = reinterpret_cast<HDC>(msg->wParam);
        if (hdc != nullptr) {
            RECT client_rect{};
            GetClientRect(hwnd_, &client_rect);
            const HBRUSH brush = CreateSolidBrush(color_);
            FillRect(hdc, &client_rect, brush);
            DeleteObject(brush);
        }

        if (result != nullptr) {
            *result = 1;
        }
        return true;
    }

private:
    HWND hwnd_ = nullptr;
    COLORREF color_ = RGB(31, 32, 36);
};

void apply_title_bar_theme(QWindow* window, Qt::ColorScheme color_scheme)
{
    if (window == nullptr) {
        return;
    }

    const BOOL use_dark_title_bar = launcher_should_use_dark_theme(color_scheme) ? TRUE : FALSE;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    constexpr DWORD kDwmWindowAttributeUseImmersiveDarkMode = 20;
    DwmSetWindowAttribute(
        hwnd,
        kDwmWindowAttributeUseImmersiveDarkMode,
        &use_dark_title_bar,
        sizeof(use_dark_title_bar));
}
#endif

} // namespace

int run_launcher_gui(int argc, char* argv[])
{
    qputenv("QT_FATAL_WARNINGS", "1");

    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("hitsc"));
    QGuiApplication::setApplicationName(QStringLiteral("hitsc"));

    const QPalette light_palette = app.palette();
    QQuickStyle::setStyle(QStringLiteral("Fusion"));
    apply_application_theme(app, light_palette, app.styleHints()->colorScheme());

    LauncherHostModel host_model;
    LauncherTheme launcher_theme(app.styleHints()->colorScheme());

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostModel"), &host_model);
    engine.rootContext()->setContextProperty(QStringLiteral("launcherTheme"), &launcher_theme);
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/Hitsc/Launcher/LauncherWindow.qml")));
    if (engine.rootObjects().isEmpty()) {
        return EXIT_FAILURE;
    }

    auto* root_window = qobject_cast<QWindow*>(engine.rootObjects().first());
    WindowPlacementController window_placement(
        root_window,
        WindowPrefsStore{},
        QStringLiteral("Launcher"));
    window_placement.restore();

#ifdef _WIN32
    LauncherBackgroundEraseFilter background_erase_filter;
    background_erase_filter.set_window(root_window);
    background_erase_filter.set_color(app.palette().color(QPalette::Window));
    app.installNativeEventFilter(&background_erase_filter);
    apply_title_bar_theme(root_window, app.styleHints()->colorScheme());
    QObject::connect(
        app.styleHints(),
        &QStyleHints::colorSchemeChanged,
        &app,
        [&app, light_palette, root_window, &launcher_theme, &background_erase_filter](
            Qt::ColorScheme color_scheme) {
            apply_application_theme(app, light_palette, color_scheme);
            launcher_theme.setColorScheme(color_scheme);
            background_erase_filter.set_color(app.palette().color(QPalette::Window));
            apply_title_bar_theme(root_window, color_scheme);
        });
#else
    QObject::connect(
        app.styleHints(),
        &QStyleHints::colorSchemeChanged,
        &app,
        [&app, light_palette, &launcher_theme](Qt::ColorScheme color_scheme) {
            apply_application_theme(app, light_palette, color_scheme);
            launcher_theme.setColorScheme(color_scheme);
        });
#endif

    return app.exec();;
}

} // namespace hitsc
