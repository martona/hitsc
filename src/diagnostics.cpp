#include "diagnostics.hpp"

#include <exception>
#include <iostream>
#include <mutex>

#ifdef _WIN32
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <vector>
#include <windows.h>
#include <dbghelp.h>
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

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* exception_info)
{
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

    std::abort();
}

} // namespace

void install_exception_handlers()
{
    std::set_terminate(terminate_handler);
#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandled_exception_filter);
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
