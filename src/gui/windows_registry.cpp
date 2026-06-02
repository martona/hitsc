#include "windows_registry.hpp"

#ifdef _WIN32

#include <cstdint>
#include <string>
#include <system_error>

namespace hitsc {

void throw_windows_error(const char* context, LONG error)
{
    throw std::system_error(
        static_cast<int>(error),
        std::system_category(),
        context);
}

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

void write_string_value(HKEY key, const wchar_t* name, const QString& value)
{
    const std::wstring wide_value = to_wide(value);
    const auto byte_count =
        static_cast<DWORD>((wide_value.size() + 1) * sizeof(wchar_t));
    const LONG result = RegSetValueExW(
        key,
        name,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(wide_value.c_str()),
        byte_count);
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to write registry string value", result);
    }
}

std::optional<QString> read_string_value(HKEY key, const wchar_t* name)
{
    DWORD type = 0;
    DWORD byte_count = 0;
    LONG result =
        RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, &type, nullptr, &byte_count);
    if (result == ERROR_FILE_NOT_FOUND) {
        return std::nullopt;
    }
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to read registry string value size", result);
    }

    std::wstring buffer(byte_count / sizeof(wchar_t), L'\0');
    result = RegGetValueW(
        key,
        nullptr,
        name,
        RRF_RT_REG_SZ,
        &type,
        buffer.data(),
        &byte_count);
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to read registry string value", result);
    }

    while (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    return from_wide(buffer);
}

void write_binary_value(HKEY key, const wchar_t* name, const QByteArray& value)
{
    const LONG result = RegSetValueExW(
        key,
        name,
        0,
        REG_BINARY,
        reinterpret_cast<const BYTE*>(value.constData()),
        static_cast<DWORD>(value.size()));
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to write registry binary value", result);
    }
}

std::optional<QByteArray> read_binary_value(HKEY key, const wchar_t* name)
{
    DWORD type = 0;
    DWORD byte_count = 0;
    LONG result =
        RegGetValueW(key, nullptr, name, RRF_RT_REG_BINARY, &type, nullptr, &byte_count);
    if (result == ERROR_FILE_NOT_FOUND) {
        return std::nullopt;
    }
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to read registry binary value size", result);
    }

    QByteArray buffer;
    buffer.resize(static_cast<qsizetype>(byte_count));
    result = RegGetValueW(
        key,
        nullptr,
        name,
        RRF_RT_REG_BINARY,
        &type,
        buffer.data(),
        &byte_count);
    if (result != ERROR_SUCCESS) {
        throw_windows_error("failed to read registry binary value", result);
    }
    buffer.resize(static_cast<qsizetype>(byte_count));
    return buffer;
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

} // namespace hitsc

#endif // _WIN32
