#include "console.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <iostream>
#include <stdexcept>
#include <string>

namespace hitsc {
namespace {

#ifdef _WIN32
class StdinModeRestore {
public:
    StdinModeRestore(HANDLE handle, DWORD mode)
        : handle_(handle), mode_(mode)
    {
    }

    StdinModeRestore(const StdinModeRestore&) = delete;
    StdinModeRestore& operator=(const StdinModeRestore&) = delete;

    ~StdinModeRestore()
    {
        SetConsoleMode(handle_, mode_);
    }

private:
    HANDLE handle_;
    DWORD mode_;
};

std::string utf8_from_wide(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }

    const int byte_count = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (byte_count <= 0) {
        throw std::runtime_error("failed to convert console input to UTF-8");
    }

    std::string result(static_cast<std::size_t>(byte_count), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        byte_count,
        nullptr,
        nullptr);
    return result;
}
#endif

} // namespace

std::string read_password_from_console(const std::string& prompt)
{
#ifdef _WIN32
    const HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD original_mode = 0;
    if (stdin_handle == INVALID_HANDLE_VALUE || !GetConsoleMode(stdin_handle, &original_mode)) {
        throw std::runtime_error(
            "cannot prompt for password because stdin is not an interactive console");
    }

    DWORD masked_mode = original_mode;
    masked_mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
    masked_mode |= ENABLE_PROCESSED_INPUT;
    if (!SetConsoleMode(stdin_handle, masked_mode)) {
        throw std::runtime_error("failed to disable console echo for password prompt");
    }
    StdinModeRestore restore(stdin_handle, original_mode);

    std::cerr << prompt;
    std::cerr.flush();

    std::wstring password;
    while (true) {
        wchar_t ch = L'\0';
        DWORD chars_read = 0;
        if (!ReadConsoleW(stdin_handle, &ch, 1, &chars_read, nullptr)) {
            throw std::runtime_error("failed to read password from console");
        }
        if (chars_read == 0) {
            continue;
        }

        if (ch == L'\r' || ch == L'\n') {
            std::cerr << "\r\n";
            return utf8_from_wide(password);
        }

        if (ch == L'\b' || ch == 0x7f) {
            if (!password.empty()) {
                password.pop_back();
                std::cerr << "\b \b";
                std::cerr.flush();
            }
            continue;
        }

        if (ch < L' ') {
            continue;
        }

        password.push_back(ch);
        std::cerr << '*';
        std::cerr.flush();
    }
#else
    (void)prompt;
    throw std::runtime_error(
        "interactive password prompting is only implemented on Windows; pass --password or --password-env");
#endif
}

} // namespace hitsc
