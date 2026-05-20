#include "parent_liveness_monitor.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <thread>
#endif

namespace hitsc {
namespace {

#ifdef _WIN32

HANDLE open_null_device()
{
    return CreateFileW(
        L"NUL",
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
}

void redirect_stream_to_null(DWORD std_handle_id, FILE* stream)
{
    const int fd = _fileno(stream);
    if (fd < 0) {
        return;
    }

    HANDLE std_handle = open_null_device();
    if (std_handle != INVALID_HANDLE_VALUE) {
        SetStdHandle(std_handle_id, std_handle);
    }

    HANDLE crt_handle = open_null_device();
    if (crt_handle == INVALID_HANDLE_VALUE) {
        return;
    }

    const int replacement_fd =
        _open_osfhandle(reinterpret_cast<intptr_t>(crt_handle), _O_TEXT);
    if (replacement_fd < 0) {
        CloseHandle(crt_handle);
        return;
    }

    _dup2(replacement_fd, fd);
    _close(replacement_fd);
}

void redirect_standard_streams_to_null()
{
    std::cout.flush();
    std::cerr.flush();
    std::clog.flush();
    std::fflush(stdout);
    std::fflush(stderr);

    redirect_stream_to_null(STD_OUTPUT_HANDLE, stdout);
    redirect_stream_to_null(STD_ERROR_HANDLE, stderr);
}

#endif

} // namespace

void ParentLivenessMonitor::start(qint64 parent_process_id)
{
#ifdef _WIN32
    if (parent_process_id <= 0
        || static_cast<DWORD>(parent_process_id) == GetCurrentProcessId()) {
        return;
    }

    HANDLE parent = OpenProcess(
        SYNCHRONIZE,
        FALSE,
        static_cast<DWORD>(parent_process_id));
    if (parent == nullptr) {
        redirect_standard_streams_to_null();
        return;
    }

    std::thread([parent] {
        WaitForSingleObject(parent, INFINITE);
        CloseHandle(parent);
        redirect_standard_streams_to_null();
    }).detach();
#else
    (void)parent_process_id;
#endif
}

} // namespace hitsc
