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
    HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD original_mode = 0;
    HANDLE console_input = INVALID_HANDLE_VALUE;  // opened only for the shim fallback
    if (stdin_handle == INVALID_HANDLE_VALUE || !GetConsoleMode(stdin_handle, &original_mode)) {
        // stdin is not a console. This is the normal case when launched via the hitsc.com shim,
        // which is /SUBSYSTEM:console and hands this GUI-subsystem child its stdio as pipes. The
        // real terminal is still reachable: attach to the parent console and open its input
        // buffer (CONIN$) directly, which GetStdHandle can't give us because stdin is redirected.
        AttachConsole(ATTACH_PARENT_PROCESS);  // no-op if already attached
        console_input = CreateFileW(
            L"CONIN$",
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (console_input == INVALID_HANDLE_VALUE || !GetConsoleMode(console_input, &original_mode)) {
            if (console_input != INVALID_HANDLE_VALUE) {
                CloseHandle(console_input);
            }
            throw std::runtime_error(
                "cannot prompt for password because no interactive console is available; "
                "pass --password-env or set HITSC_PASSWORD");
        }
        stdin_handle = console_input;
    }

    // Close the CONIN$ handle (if we opened one) when the prompt returns or throws.
    struct ConsoleInputCloser {
        HANDLE handle;
        ~ConsoleInputCloser()
        {
            if (handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
            }
        }
    } console_input_closer{console_input};

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
