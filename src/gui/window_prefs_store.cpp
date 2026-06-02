#include "window_prefs_store.hpp"

#include "errors.hpp"

#include <utility>

#ifdef _WIN32
#include "windows_registry.hpp"

#include <windows.h>
#else
#include <QSettings>
#endif

namespace hitsc {
namespace {

#ifdef _WIN32

QString window_key_path(const QString& root_path, const QString& window_name)
{
    const QString trimmed_name = window_name.trimmed();
    if (trimmed_name.isEmpty()) {
        throw UserError("window preference name is empty");
    }
    return root_path + QStringLiteral("\\Prefs\\Windows\\") + trimmed_name;
}

#else

QString settings_group(const QString& window_name)
{
    const QString trimmed_name = window_name.trimmed();
    if (trimmed_name.isEmpty()) {
        throw UserError("window preference name is empty");
    }
    return QStringLiteral("Prefs/Windows/") + trimmed_name;
}

#endif

} // namespace

WindowPrefsStore::WindowPrefsStore(QString root_path)
    : root_path_(std::move(root_path))
{
}

const QString& WindowPrefsStore::root_path() const
{
    return root_path_;
}

#ifdef _WIN32

std::optional<QRect> WindowPrefsStore::load_window_rect(const QString& window_name) const
{
    const auto key = open_key(
        HKEY_CURRENT_USER,
        window_key_path(root_path_, window_name),
        KEY_READ);
    if (!key) {
        return std::nullopt;
    }

    const auto x = read_int_value(key->get(), L"X");
    const auto y = read_int_value(key->get(), L"Y");
    const auto width = read_int_value(key->get(), L"Width");
    const auto height = read_int_value(key->get(), L"Height");
    if (!x || !y || !width || !height) {
        return std::nullopt;
    }

    return QRect(*x, *y, *width, *height);
}

void WindowPrefsStore::save_window_rect(const QString& window_name, const QRect& rect) const
{
    if (rect.width() <= 0 || rect.height() <= 0) {
        return;
    }

    const RegistryKey key = create_key(
        HKEY_CURRENT_USER,
        window_key_path(root_path_, window_name));
    write_int_value(key.get(), L"X", rect.x());
    write_int_value(key.get(), L"Y", rect.y());
    write_int_value(key.get(), L"Width", rect.width());
    write_int_value(key.get(), L"Height", rect.height());
}

#else

std::optional<QRect> WindowPrefsStore::load_window_rect(const QString& window_name) const
{
    (void)root_path_;
    QSettings settings(QStringLiteral("hitsc"), QStringLiteral("hitsc"));
    settings.beginGroup(settings_group(window_name));

    const QVariant x = settings.value(QStringLiteral("X"));
    const QVariant y = settings.value(QStringLiteral("Y"));
    const QVariant width = settings.value(QStringLiteral("Width"));
    const QVariant height = settings.value(QStringLiteral("Height"));
    if (!x.isValid() || !y.isValid() || !width.isValid() || !height.isValid()) {
        return std::nullopt;
    }

    return QRect(x.toInt(), y.toInt(), width.toInt(), height.toInt());
}

void WindowPrefsStore::save_window_rect(const QString& window_name, const QRect& rect) const
{
    (void)root_path_;
    if (rect.width() <= 0 || rect.height() <= 0) {
        return;
    }

    QSettings settings(QStringLiteral("hitsc"), QStringLiteral("hitsc"));
    settings.beginGroup(settings_group(window_name));
    settings.setValue(QStringLiteral("X"), rect.x());
    settings.setValue(QStringLiteral("Y"), rect.y());
    settings.setValue(QStringLiteral("Width"), rect.width());
    settings.setValue(QStringLiteral("Height"), rect.height());
}

#endif

} // namespace hitsc
