#include "launcher_theme.hpp"

#include <QSettings>

namespace hitsc {
namespace {

#ifdef _WIN32
bool windows_apps_use_dark_theme()
{
    const QSettings theme_settings(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat);
    return theme_settings.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0;
}
#endif

} // namespace

bool launcher_should_use_dark_theme(Qt::ColorScheme color_scheme)
{
    if (color_scheme == Qt::ColorScheme::Dark) {
        return true;
    }
    if (color_scheme == Qt::ColorScheme::Light) {
        return false;
    }

#ifdef _WIN32
    return windows_apps_use_dark_theme();
#else
    return false;
#endif
}

LauncherTheme::LauncherTheme(Qt::ColorScheme color_scheme, QObject* parent)
    : QObject(parent)
    , dark_mode_(launcher_should_use_dark_theme(color_scheme))
{
}

bool LauncherTheme::darkMode() const
{
    return dark_mode_;
}

void LauncherTheme::setColorScheme(Qt::ColorScheme color_scheme)
{
    const bool next_dark_mode = launcher_should_use_dark_theme(color_scheme);
    if (dark_mode_ == next_dark_mode) {
        return;
    }

    dark_mode_ = next_dark_mode;
    emit darkModeChanged();
}

} // namespace hitsc
