#pragma once

#ifdef _WIN32

#include "qt_win_string.hpp"

#include <windows.h>

#include <QByteArray>
#include <QString>

#include <optional>

namespace hitsc {

// Win32 registry helpers shared by the launcher persistence stores. The whole
// header is gated on _WIN32 so it can be included unconditionally on any
// platform; on non-Windows it expands to nothing. QString/wide-string
// conversion (to_wide/from_wide) comes from qt_win_string.hpp.

[[noreturn]] void throw_windows_error(const char* context, LONG error);

// RAII owner for an HKEY that closes the key on destruction.
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

// Create (opening if it already exists) a read/write subkey under parent.
RegistryKey create_key(HKEY parent, const QString& path);

// Open an existing subkey; returns nullopt if it does not exist.
std::optional<RegistryKey> open_key(HKEY parent, const QString& path, REGSAM access);

void write_string_value(HKEY key, const wchar_t* name, const QString& value);
std::optional<QString> read_string_value(HKEY key, const wchar_t* name);

void write_binary_value(HKEY key, const wchar_t* name, const QByteArray& value);
std::optional<QByteArray> read_binary_value(HKEY key, const wchar_t* name);

void write_int_value(HKEY key, const wchar_t* name, int value);
std::optional<int> read_int_value(HKEY key, const wchar_t* name);

} // namespace hitsc

#endif // _WIN32
