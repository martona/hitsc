#include "window_prefs_store.hpp"

#include "errors.hpp"

#include <utility>

#ifdef _WIN32
#include <cstdint>
#include <string>
#include <system_error>

#include <windows.h>
#else
#include <QSettings>
#endif

namespace hitsc {
namespace {

#ifdef _WIN32

std::wstring to_wide(const QString& value)
{
    return std::wstring(
        reinterpret_cast<const wchar_t*>(value.utf16()),
        static_cast<std::size_t>(value.size()));
}

[[noreturn]] void throw_windows_error(const char* context, LONG error)
{
    throw std::system_error(
        static_cast<int>(error),
        std::system_category(),
        context);
}

class RegistryKey {
public:
    RegistryKey() = default;
    explicit RegistryKey(HKEY key)
        : key_(key)
    {
    }

    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    RegistryKey(RegistryKey&& other) noexcept
        : key_(other.key_)
    {
        other.key_ = nullptr;
    }

    RegistryKey& operator=(RegistryKey&& other) noexcept
    {
        if (this != &other) {
            close();
            key_ = other.key_;
            other.key_ = nullptr;
        }
        return *this;
    }

    ~RegistryKey()
    {
        close();
    }

    HKEY get() const
    {
        return key_;
    }

private:
    void close()
    {
        if (key_ != nullptr) {
            RegCloseKey(key_);
            key_ = nullptr;
        }
    }

    HKEY key_ = nullptr;
};

RegistryKey create_key(HKEY parent, const QString& path)
{
    HKEY key = nullptr;
    const std::wstring wide_path = to_wide(path);
    const LONG result = RegCreateKeyExW(
        parent,
        wide_path.c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_READ | KEY_WRITE,
        nullptr,
        &key,
        nullptr);
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to create registry key", result);
    }
    return RegistryKey(key);
}

std::optional<RegistryKey> open_key(HKEY parent, const QString& path, REGSAM access)
{
    HKEY key = nullptr;
    const std::wstring wide_path = to_wide(path);
    const LONG result = RegOpenKeyExW(parent, wide_path.c_str(), 0, access, &key);
    if (result == ERROR_FILE_NOT_FOUND) {
        return std::nullopt;
    }
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to open registry key", result);
    }
    return RegistryKey(key);
}

void write_int_value(HKEY key, const wchar_t* name, int value)
{
    const DWORD stored_value = static_cast<DWORD>(value);
    const LONG result = RegSetValueExW(
        key,
        name,
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&stored_value),
        sizeof(stored_value));
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to write registry integer value", result);
    }
}

std::optional<int> read_int_value(HKEY key, const wchar_t* name)
{
    DWORD type = 0;
    DWORD value = 0;
    DWORD byte_count = sizeof(value);
    const LONG result = RegGetValueW(
        key,
        nullptr,
        name,
        RRF_RT_REG_DWORD,
        &type,
        &value,
        &byte_count);
    if (result == ERROR_FILE_NOT_FOUND) {
        return std::nullopt;
    }
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to read registry integer value", result);
    }
    return static_cast<int>(static_cast<std::int32_t>(value));
}

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
