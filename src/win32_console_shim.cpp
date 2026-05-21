#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kConsoleShimEnvironmentName[] = L"HITSC_CONSOLE_SHIM";

BOOL WINAPI ignore_console_control(DWORD)
{
    return TRUE;
}

std::wstring executable_path_for_shim()
{
    std::vector<wchar_t> buffer(MAX_PATH);

    while (true) {
        const DWORD copied =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buffer.size() - 1) {
            std::wstring path(buffer.data(), copied);
            const auto dot = path.find_last_of(L'.');
            if (dot == std::wstring::npos) {
                path += L".exe";
            } else {
                path.replace(dot, std::wstring::npos, L".exe");
            }
            return path;
        }

        buffer.resize(buffer.size() * 2);
    }
}

HANDLE duplicate_inheritable(HANDLE handle)
{
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return nullptr;
    }

    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(
            GetCurrentProcess(),
            handle,
            GetCurrentProcess(),
            &duplicate,
            0,
            TRUE,
            DUPLICATE_SAME_ACCESS)) {
        return nullptr;
    }
    return duplicate;
}

class UniqueHandle {
public:
    UniqueHandle() = default;

    explicit UniqueHandle(HANDLE handle)
        : handle_(handle)
    {
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    ~UniqueHandle()
    {
        reset();
    }

    HANDLE get() const
    {
        return handle_;
    }

    bool valid() const
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE handle = nullptr)
    {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

struct Pipe {
    UniqueHandle read;
    UniqueHandle write;
};

Pipe create_inheritable_pipe()
{
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!CreatePipe(&read, &write, &attributes, 0)) {
        return {};
    }

    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    return {UniqueHandle(read), UniqueHandle(write)};
}

void write_all(HANDLE handle, const char* data, DWORD size)
{
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(handle, data, size, &written, nullptr) || written == 0) {
            return;
        }
        data += written;
        size -= written;
    }
}

void pump_output(UniqueHandle read_handle, HANDLE write_handle)
{
    if (!read_handle.valid()
        || write_handle == nullptr
        || write_handle == INVALID_HANDLE_VALUE) {
        return;
    }

    char buffer[4096];
    while (true) {
        DWORD bytes_read = 0;
        if (!ReadFile(read_handle.get(), buffer, sizeof(buffer), &bytes_read, nullptr)
            || bytes_read == 0) {
            return;
        }
        write_all(write_handle, buffer, bytes_read);
    }
}

class ChildStdio {
public:
    ChildStdio()
        : stdin_(duplicate_inheritable(GetStdHandle(STD_INPUT_HANDLE))),
          stdout_pipe_(create_inheritable_pipe()),
          stderr_pipe_(create_inheritable_pipe()),
          parent_stdout_(GetStdHandle(STD_OUTPUT_HANDLE)),
          parent_stderr_(GetStdHandle(STD_ERROR_HANDLE))
    {
    }

    ChildStdio(const ChildStdio&) = delete;
    ChildStdio& operator=(const ChildStdio&) = delete;

    ~ChildStdio()
    {
        join_pumps();
    }

    bool ready() const
    {
        return stdout_pipe_.read.valid()
            && stdout_pipe_.write.valid()
            && stderr_pipe_.read.valid()
            && stderr_pipe_.write.valid();
    }

    void apply_to(STARTUPINFOW& startup_info) const
    {
        startup_info.dwFlags |= STARTF_USESTDHANDLES;
        startup_info.hStdInput = stdin_.valid() ? stdin_.get() : GetStdHandle(STD_INPUT_HANDLE);
        startup_info.hStdOutput = stdout_pipe_.write.get();
        startup_info.hStdError = stderr_pipe_.write.get();
    }

    void close_child_ends()
    {
        stdout_pipe_.write.reset();
        stderr_pipe_.write.reset();
    }

    void start_pumps()
    {
        stdout_thread_ =
            std::thread(pump_output, std::move(stdout_pipe_.read), parent_stdout_);
        stderr_thread_ =
            std::thread(pump_output, std::move(stderr_pipe_.read), parent_stderr_);
    }

    void join_pumps()
    {
        if (stdout_thread_.joinable()) {
            stdout_thread_.join();
        }
        if (stderr_thread_.joinable()) {
            stderr_thread_.join();
        }
    }

private:
    UniqueHandle stdin_;
    Pipe stdout_pipe_;
    Pipe stderr_pipe_;
    HANDLE parent_stdout_ = nullptr;
    HANDLE parent_stderr_ = nullptr;
    std::thread stdout_thread_;
    std::thread stderr_thread_;
};

} // namespace

int main()
{
    SetConsoleCtrlHandler(ignore_console_control, TRUE);

    const std::wstring exe_path = executable_path_for_shim();
    if (exe_path.empty()) {
        std::fwprintf(stderr, L"hitsc.com: failed to resolve executable path\n");
        return 1;
    }

    std::wstring command_line = GetCommandLineW();
    ChildStdio stdio;
    if (!stdio.ready()) {
        std::fwprintf(stderr, L"hitsc.com: failed to create stdio pipes\n");
        return 1;
    }
    if (!SetEnvironmentVariableW(kConsoleShimEnvironmentName, L"1")) {
        std::fwprintf(stderr, L"hitsc.com: failed to set child environment\n");
        return 1;
    }

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    stdio.apply_to(startup_info);

    PROCESS_INFORMATION process_info{};
    if (!CreateProcessW(
            exe_path.c_str(),
            command_line.data(),
            nullptr,
            nullptr,
            TRUE,
            0,
            nullptr,
            nullptr,
            &startup_info,
            &process_info)) {
        const DWORD error = GetLastError();
        std::fwprintf(
            stderr,
            L"hitsc.com: failed to launch %ls (Windows error %lu)\n",
            exe_path.c_str(),
            static_cast<unsigned long>(error));
        return 1;
    }

    stdio.close_child_ends();
    stdio.start_pumps();

    WaitForSingleObject(process_info.hProcess, INFINITE);

    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process_info.hProcess, &exit_code)) {
        exit_code = 1;
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    stdio.join_pumps();

    return static_cast<int>(exit_code);
}
