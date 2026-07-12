#include "launcher_gui.hpp"

#include "launcher_host_model.hpp"
#include "launcher_theme.hpp"
#include "window_placement.hpp"
#include "window_prefs_store.hpp"

#include <QColor>
#include <QGuiApplication>
#include <QIcon>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSize>
#include <QStyleHints>
#include <QUrl>
#include <QVariant>
#include <QWindow>

#include <QWKQuick/quickwindowagent.h>

#include <cstdlib>

#ifdef _WIN32
#include <QAbstractNativeEventFilter>

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

QIcon make_application_icon()
{
    QIcon icon;
    icon.addFile(QStringLiteral(":/icons/hitsc-16.png"), QSize(16, 16));
    icon.addFile(QStringLiteral(":/icons/hitsc-24.png"), QSize(24, 24));
    icon.addFile(QStringLiteral(":/icons/hitsc-32.png"), QSize(32, 32));
    icon.addFile(QStringLiteral(":/icons/hitsc-48.png"), QSize(48, 48));
    icon.addFile(QStringLiteral(":/icons/hitsc-64.png"), QSize(64, 64));
    icon.addFile(QStringLiteral(":/icons/hitsc-128.png"), QSize(128, 128));
    icon.addFile(QStringLiteral(":/icons/hitsc-256.png"), QSize(256, 256));
    icon.addFile(QStringLiteral(":/icons/hitsc-512.png"), QSize(512, 512));
    return icon;
}

void apply_application_theme(QGuiApplication& app, const QPalette& light_palette, Qt::ColorScheme color_scheme)
{
    const bool dark = launcher_should_use_dark_theme(color_scheme);
    app.styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    app.setPalette(dark ? make_dark_palette() : light_palette);
}

void apply_window_clear_color(QWindow* window, const QColor& color)
{
    auto* quick_window = qobject_cast<QQuickWindow*>(window);
    if (quick_window != nullptr) {
        quick_window->setColor(color);
    }
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

#endif

} // namespace

int run_launcher_gui(int argc, char* argv[], VerbosityOptions verbosity)
{
    // Make Qt warnings fatal ONLY in debug builds. It promotes every qWarning() to qAbort(), which
    // is a useful dev aid for catching QML binding errors etc. immediately -- but a hard liability
    // in a shipped app, because it turns benign, transient, uncontrollable warnings into a crash.
    // The one that bit us: a focused QML TextField re-reads the clipboard via its canPaste property
    // on every WM_CLIPBOARDUPDATE broadcast, so when another app (mstsc/RDP, etc.) momentarily holds
    // the clipboard open, OleGetClipboard fails, Qt warns, and QT_FATAL_WARNINGS kills the launcher
    // -- with no copy/paste on our part. So: on in Debug, never forced in Release. (The child process
    // env already strips this var for the same reason.) A developer on a non-Debug build can still
    // opt in by setting QT_FATAL_WARNINGS=1 in the environment; Qt honors it natively.
#ifdef _DEBUG
    qputenv("QT_FATAL_WARNINGS", "1");
#endif

    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("hitsc"));
    QGuiApplication::setApplicationName(QStringLiteral("hitsc"));
    QGuiApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QGuiApplication::setWindowIcon(make_application_icon());

    const QPalette light_palette = app.palette();
    QQuickStyle::setStyle(QStringLiteral("Fusion"));
    apply_application_theme(app, light_palette, app.styleHints()->colorScheme());

    LauncherHostModel host_model(verbosity);
    LauncherTheme launcher_theme(app.styleHints()->colorScheme());
    WindowPlacementController window_placement(WindowPrefsStore{}, QStringLiteral("Launcher"));

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostModel"), &host_model);
    engine.rootContext()->setContextProperty(QStringLiteral("launcherTheme"), &launcher_theme);
    engine.rootContext()->setContextProperty(QStringLiteral("windowPlacement"), &window_placement);
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/Hitsc/Launcher/LauncherWindow.qml")));
    if (engine.rootObjects().isEmpty()) {
        return EXIT_FAILURE;
    }

    auto* root_window = qobject_cast<QWindow*>(engine.rootObjects().first());
    if (root_window != nullptr) {
        root_window->setIcon(QGuiApplication::windowIcon());
    }
    apply_window_clear_color(root_window, app.palette().color(QPalette::Window));

    // Take the non-client frame from the OS and draw our own caption (QWindowKit
    // keeps native snap/shadow/resize). The caption is the QML ToolBar tagged
    // objectName "titleBar"; its interactive children are registered so clicks
    // reach them instead of dragging the window.
    auto* quick_window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* window_agent = new QWK::QuickWindowAgent(quick_window);
    if (quick_window != nullptr) {
        window_agent->setup(quick_window);
        if (auto* title_bar = quick_window->findChild<QQuickItem*>(QStringLiteral("titleBar"))) {
            window_agent->setTitleBar(title_bar);
            if (auto* item = quick_window->findChild<QQuickItem*>(QStringLiteral("windowIcon"))) {
                window_agent->setSystemButton(QWK::WindowAgentBase::WindowIcon, item);
            }
            if (auto* item = quick_window->findChild<QQuickItem*>(QStringLiteral("menuButton"))) {
                window_agent->setHitTestVisible(item, true);
            }
            if (auto* item = quick_window->findChild<QQuickItem*>(QStringLiteral("viewToggleButton"))) {
                window_agent->setHitTestVisible(item, true);
            }
            if (auto* item = quick_window->findChild<QQuickItem*>(QStringLiteral("minButton"))) {
                window_agent->setSystemButton(QWK::WindowAgentBase::Minimize, item);
            }
            if (auto* item = quick_window->findChild<QQuickItem*>(QStringLiteral("maxButton"))) {
                window_agent->setSystemButton(QWK::WindowAgentBase::Maximize, item);
            }
            if (auto* item = quick_window->findChild<QQuickItem*>(QStringLiteral("closeButton"))) {
                window_agent->setSystemButton(QWK::WindowAgentBase::Close, item);
            }
        }
        window_agent->setWindowAttribute(
            QStringLiteral("dark-mode"),
            launcher_should_use_dark_theme(app.styleHints()->colorScheme()));
    }

    window_placement.attach(root_window);
    window_placement.restore();

    QObject::connect(
        &app,
        &QGuiApplication::aboutToQuit,
        &window_placement,
        &WindowPlacementController::save);

#ifdef _WIN32
    LauncherBackgroundEraseFilter background_erase_filter;
    background_erase_filter.set_window(root_window);
    background_erase_filter.set_color(app.palette().color(QPalette::Window));
    app.installNativeEventFilter(&background_erase_filter);
    QObject::connect(
        app.styleHints(),
        &QStyleHints::colorSchemeChanged,
        &app,
        [&app, light_palette, root_window, &launcher_theme, &background_erase_filter, window_agent](
            Qt::ColorScheme color_scheme) {
            apply_application_theme(app, light_palette, color_scheme);
            launcher_theme.setColorScheme(color_scheme);
            apply_window_clear_color(root_window, app.palette().color(QPalette::Window));
            background_erase_filter.set_color(app.palette().color(QPalette::Window));
            window_agent->setWindowAttribute(
                QStringLiteral("dark-mode"), launcher_should_use_dark_theme(color_scheme));
        });
#else
    QObject::connect(
        app.styleHints(),
        &QStyleHints::colorSchemeChanged,
        &app,
        [&app, light_palette, root_window, &launcher_theme, window_agent](Qt::ColorScheme color_scheme) {
            apply_application_theme(app, light_palette, color_scheme);
            launcher_theme.setColorScheme(color_scheme);
            apply_window_clear_color(root_window, app.palette().color(QPalette::Window));
            window_agent->setWindowAttribute(
                QStringLiteral("dark-mode"), launcher_should_use_dark_theme(color_scheme));
        });
#endif

    root_window->show();

    const int exit_code = app.exec();

    // Detach before the QML engine (declared after this controller, hence
    // destroyed first) tears down the window. Otherwise ~WindowPlacementController
    // would call removeEventFilter on an already-freed window.
    window_placement.attach(nullptr);

    return exit_code;
}

} // namespace hitsc
