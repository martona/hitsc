#include "diagnostics.hpp"

#include <exception>
#include <iostream>
#include <mutex>

#ifdef _WIN32
#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <iomanip>
#include <sstream>
#include <vector>
#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>
#else
#include <cstdlib>
#endif

namespace hitsc {
namespace {

#ifdef _WIN32

std::mutex& symbol_mutex()
{
    static std::mutex mutex;
    return mutex;
}

void initialize_symbols_locked()
{
    static bool initialized = false;
    if (initialized) {
        return;
    }

    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);
    initialized = true;
}

void print_symbolized_address_locked(std::ostream& output, std::size_t index, DWORD64 address)
{
    HANDLE process = GetCurrentProcess();

    output << "  #" << index << " 0x" << std::hex << address << std::dec;

    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    if (SymGetModuleInfo64(process, address, &module)) {
        output << " " << module.ModuleName
               << "+0x" << std::hex << (address - module.BaseOfImage) << std::dec;
    }

    alignas(SYMBOL_INFO) std::array<unsigned char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbol_storage{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_storage.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    DWORD64 displacement = 0;
    if (SymFromAddr(process, address, &displacement, symbol)) {
        output << " " << symbol->Name;
        if (displacement != 0) {
            output << "+0x" << std::hex << displacement << std::dec;
        }
    }

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD line_displacement = 0;
    if (SymGetLineFromAddr64(process, address, &line_displacement, &line)) {
        output << " (" << line.FileName << ':' << line.LineNumber << ')';
    }

    output << '\n';
}

void print_context_stack_trace(std::ostream& output, CONTEXT* context)
{
    if (context == nullptr) {
        print_stack_trace(output, "SEH handler", 0);
        return;
    }

    std::lock_guard lock(symbol_mutex());
    initialize_symbols_locked();

    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    STACKFRAME64 frame{};
    DWORD machine_type = 0;

#if defined(_M_X64)
    machine_type = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context->Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context->Rsp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_IX86)
    machine_type = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context->Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context->Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Esp;
    frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_ARM64)
    machine_type = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context->Pc;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context->Fp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Sp;
    frame.AddrStack.Mode = AddrModeFlat;
#else
    print_stack_trace(output, "SEH handler", 0);
    return;
#endif

    output << "hitsc: stack trace:\n";
    for (std::size_t index = 0; index < 64; ++index) {
        if (frame.AddrPC.Offset == 0) {
            break;
        }

        print_symbolized_address_locked(output, index, frame.AddrPC.Offset);

        if (!StackWalk64(
                machine_type,
                process,
                thread,
                &frame,
                context,
                nullptr,
                SymFunctionTableAccess64,
                SymGetModuleBase64,
                nullptr)) {
            break;
        }
    }
}

// %USERPROFILE%\AppData\LocalLow\hitsc -- created on demand. LocalLow so even a
// crashing low-integrity process could write here.
std::wstring crash_report_directory()
{
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, KF_FLAG_CREATE, nullptr, &base))
        || base == nullptr) {
        return {};
    }
    std::wstring directory(base);
    CoTaskMemFree(base);
    directory += L"\\hitsc";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory;
}

// Writes hitsc-crash-<date>-<time>-<pid>.dmp (minidump) and .txt (reason, command
// line, exception code, symbolized stack) for post-mortem debugging. The launcher
// runs windowless with no console, so stderr traces vanish -- these files are the
// only artifact of a crash. Best-effort by design: it runs on the crashing thread,
// so any failure just means no report. The first crasher wins; a fault while
// reporting (or a second thread crashing) returns immediately.
void write_crash_report(EXCEPTION_POINTERS* exception_info, const char* reason)
{
    static std::atomic_bool writing{false};
    if (writing.exchange(true)) {
        return;
    }

    const std::wstring directory = crash_report_directory();
    if (directory.empty()) {
        return;
    }

    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t name[128];
    _snwprintf_s(
        name,
        _TRUNCATE,
        L"\\hitsc-crash-%04u%02u%02u-%02u%02u%02u-%lu",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        GetCurrentProcessId());
    const std::wstring base_path = directory + name;

    const std::wstring dump_path = base_path + L".dmp";
    HANDLE file = CreateFileW(
        dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception_param{};
        exception_param.ThreadId = GetCurrentThreadId();
        exception_param.ExceptionPointers = exception_info;
        exception_param.ClientPointers = FALSE;
        const auto dump_type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo
            | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);
        MiniDumpWriteDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            file,
            dump_type,
            exception_info != nullptr ? &exception_param : nullptr,
            nullptr,
            nullptr);
        CloseHandle(file);
    }

    try {
        std::ofstream report((base_path + L".txt").c_str());
        if (!report) {
            return;
        }
        report << "hitsc crash report\n"
               << "reason: " << reason << '\n'
               << "command line: " << GetCommandLineA() << '\n';
        if (exception_info != nullptr && exception_info->ExceptionRecord != nullptr) {
            report << "exception code: 0x" << std::hex
                   << exception_info->ExceptionRecord->ExceptionCode << std::dec
                   << " address: " << exception_info->ExceptionRecord->ExceptionAddress << '\n';
        }
        if (const std::exception_ptr current = std::current_exception()) {
            try {
                std::rethrow_exception(current);
            } catch (const std::exception& ex) {
                report << "current exception: " << ex.what() << '\n';
            } catch (...) {
                report << "current exception: non-standard exception\n";
            }
        }
        if (exception_info != nullptr) {
            print_context_stack_trace(report, exception_info->ContextRecord);
        } else {
            print_stack_trace(report, reason, 1);
        }
    } catch (...) {
    }
}

extern "C" void abort_signal_handler(int)
{
    // qFatal/assert/CRT paths reach process death through abort(), which never
    // passes the unhandled-exception filter -- catch it here. No-op if a crash
    // report is already being written (e.g. terminate_handler's abort).
    write_crash_report(nullptr, "abort (SIGABRT)");
}

void purecall_handler()
{
    write_crash_report(nullptr, "pure virtual call");
    std::abort();
}

void invalid_parameter_handler(
    const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
    write_crash_report(nullptr, "CRT invalid parameter");
    std::abort();
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* exception_info)
{
    write_crash_report(exception_info, "unhandled SEH exception");
    try {
        const DWORD code = exception_info && exception_info->ExceptionRecord
            ? exception_info->ExceptionRecord->ExceptionCode
            : 0;
        const void* address = exception_info && exception_info->ExceptionRecord
            ? exception_info->ExceptionRecord->ExceptionAddress
            : nullptr;

        std::cerr << "hitsc: unhandled Windows exception"
                  << " code=0x" << std::hex << code << std::dec
                  << " address=" << address << '\n';
        print_context_stack_trace(
            std::cerr,
            exception_info == nullptr ? nullptr : exception_info->ContextRecord);
    } catch (...) {
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

#endif

void terminate_handler() noexcept
{
    try {
        std::cerr << "hitsc: std::terminate called\n";
        print_current_exception_with_stack(std::cerr, "terminate");
    } catch (...) {
    }

#ifdef _WIN32
    write_crash_report(nullptr, "std::terminate");
#endif
    std::abort();
}

} // namespace

void install_exception_handlers()
{
    std::set_terminate(terminate_handler);
#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandled_exception_filter);
    std::signal(SIGABRT, abort_signal_handler);
    _set_purecall_handler(purecall_handler);
    _set_invalid_parameter_handler(invalid_parameter_handler);
#endif
}

void print_stack_trace(std::ostream& output, std::string_view context, unsigned frames_to_skip)
{
    if (!context.empty()) {
        output << "hitsc: stack trace for " << context << ":\n";
    } else {
        output << "hitsc: stack trace:\n";
    }

#ifdef _WIN32
    std::array<void*, 64> frames{};
    const USHORT captured = CaptureStackBackTrace(
        frames_to_skip + 1,
        static_cast<DWORD>(frames.size()),
        frames.data(),
        nullptr);

    std::lock_guard lock(symbol_mutex());
    initialize_symbols_locked();
    for (USHORT index = 0; index < captured; ++index) {
        print_symbolized_address_locked(output, index, reinterpret_cast<DWORD64>(frames[index]));
    }
#else
    (void)frames_to_skip;
    output << "  stack traces are not implemented on this platform yet\n";
#endif
}

void print_exception_with_stack(std::ostream& output, const std::exception& ex, std::string_view context)
{
    output << "hitsc: exception";
    if (!context.empty()) {
        output << " in " << context;
    }
    output << ": " << ex.what() << '\n';
    print_stack_trace(output, context, 1);
}

void print_current_exception_with_stack(std::ostream& output, std::string_view context)
{
    const std::exception_ptr current = std::current_exception();
    if (!current) {
        output << "hitsc: exception";
        if (!context.empty()) {
            output << " in " << context;
        }
        output << ": unknown exception\n";
        print_stack_trace(output, context, 1);
        return;
    }

    try {
        std::rethrow_exception(current);
    } catch (const std::exception& ex) {
        print_exception_with_stack(output, ex, context);
    } catch (...) {
        output << "hitsc: exception";
        if (!context.empty()) {
            output << " in " << context;
        }
        output << ": non-standard exception\n";
        print_stack_trace(output, context, 1);
    }
}

} // namespace hitsc
