#pragma once

#ifdef _WIN32

#include <QString>

#include <cstddef>
#include <string>

namespace hitsc {

// Convert between QString and the UTF-16 std::wstring expected by Win32 wide
// ("...W") APIs. Header-only so every translation unit doing Win32 interop
// shares a single definition.

inline std::wstring to_wide(const QString& value)
{
    return std::wstring(
        reinterpret_cast<const wchar_t*>(value.utf16()),
        static_cast<std::size_t>(value.size()));
}

inline QString from_wide(const std::wstring& value)
{
    return QString::fromWCharArray(value.c_str(), static_cast<int>(value.size()));
}

} // namespace hitsc

#endif // _WIN32
