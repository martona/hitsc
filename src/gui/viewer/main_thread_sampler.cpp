#include "gui/viewer/main_thread_sampler.hpp"

#if defined(HITSC_DEBUG_MAIN_SAMPLER) && defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#pragma comment(lib, "dbghelp.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace hitsc {
namespace {

constexpr int kMaxFrames = 48;
constexpr auto kSampleInterval = std::chrono::milliseconds(5); // ~200 Hz
constexpr auto kLogInterval = std::chrono::seconds(2);
constexpr int kTopStacks = 6;
constexpr int kPrintFrames = 18;

// Walk the SUSPENDED thread's stack with the x64 unwinder, copying return
// addresses into `out` (returns the count). POD-only with SEH -- no C++ objects
// with destructors live here, so __try/__except is legal -- and crucially it does
// NOT allocate: we must never touch the heap while the target is suspended, or we
// deadlock against the heap lock it may be holding. Symbolization happens later,
// once the target is running again.
int capture_stack(const CONTEXT& start, DWORD64* out, int max_frames)
{
    CONTEXT ctx = start; // local copy: RtlVirtualUnwind mutates it, and a local is aligned
    int n = 0;
    __try {
        while (n < max_frames && ctx.Rip != 0) {
            out[n++] = ctx.Rip;

            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
            if (entry != nullptr) {
                PVOID handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, entry, &ctx,
                                 &handler_data, &establisher, nullptr);
            } else {
                // Leaf frame (no unwind info): return address sits at [Rsp].
                if (ctx.Rsp == 0) {
                    break;
                }
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += sizeof(DWORD64);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Caught the thread in a spot we can't unwind -- keep the frames we have.
    }
    return n;
}

ULONGLONG filetime_to_100ns(const FILETIME& ft)
{
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

// Resolve a thread's Win32 start address to a symbol (what Process Explorer /
// SystemInformer show in their "Start address" column), so a hot thread names
// itself instead of us guessing. Best-effort; writes "" on failure.
void thread_start_symbol(HANDLE process, DWORD tid, char* out, std::size_t out_size)
{
    out[0] = '\0';
    using NtQueryInformationThread_t =
        LONG(NTAPI*)(HANDLE, int /*ThreadInfoClass*/, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<NtQueryInformationThread_t>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    if (query == nullptr) {
        return;
    }

    HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
    if (th == nullptr) {
        return;
    }
    DWORD64 start_address = 0;
    // ThreadQuerySetWin32StartAddress == 9
    query(th, 9, &start_address, sizeof(start_address), nullptr);
    CloseHandle(th);
    if (start_address == 0) {
        return;
    }

    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 512];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    IMAGEHLP_MODULE64 module_info;
    module_info.SizeOfStruct = sizeof(module_info);
    const char* module_name = SymGetModuleInfo64(process, start_address, &module_info)
        ? module_info.ModuleName
        : "?";
    DWORD64 displacement = 0;
    if (SymFromAddr(process, start_address, &displacement, symbol)) {
        std::snprintf(out, out_size, "%s!%s", module_name, symbol->Name);
    } else {
        std::snprintf(out, out_size, "%s+0x%llx", module_name,
                      static_cast<unsigned long long>(start_address));
    }
}

} // namespace

struct MainThreadSampler::Impl {
    HANDLE thread = nullptr;
    std::atomic_bool stop{false};
    std::thread sampler;

    std::mutex mutex;
    std::map<std::vector<DWORD64>, std::uint32_t> histogram;
    std::uint64_t total_samples = 0;

    // Per-thread CPU accounting. A user-mode stack walk can't see kernel CPU, and we
    // can't assume the main thread is even the hot one -- so each window we read
    // EVERY thread's kernel/user time and report the busiest, which is the only way
    // to be sure which thread is burning the core (and whether it's kernel or user).
    DWORD main_tid = 0;
    std::map<DWORD, std::pair<ULONGLONG, ULONGLONG>> prev_thread_times; // tid -> (kernel,user) 100ns
    ULONGLONG prev_process_100ns = 0; // whole-process kernel+user, to catch exited threads

    void run()
    {
        const HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(process, nullptr, TRUE);

        auto last_log = std::chrono::steady_clock::now();
        while (!stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(kSampleInterval);

            DWORD64 frames[kMaxFrames];
            int n = 0;
            if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                CONTEXT ctx;
                ctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(thread, &ctx)) {
                    n = capture_stack(ctx, frames, kMaxFrames);
                }
                ResumeThread(thread); // resume BEFORE the allocation below
            }

            if (n > 0) {
                // Target is running again -- safe to hit the heap now.
                std::vector<DWORD64> key(frames, frames + n);
                std::lock_guard<std::mutex> lock(mutex);
                ++histogram[std::move(key)];
                ++total_samples;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - last_log >= kLogInterval) {
                log_top(process, std::chrono::duration<double>(now - last_log).count());
                last_log = now;
            }
        }

        SymCleanup(process);
    }

    // Read EVERY thread in the process, diff its kernel/user time over the window,
    // and print the busiest -- this is the load-bearing output: it finds the hot
    // thread without assuming it's the main one, and splits kernel vs user (a
    // user-mode stack walk is blind to kernel CPU). The main/event-loop thread is
    // tagged so we can see at a glance whether it's even involved.
    void report_threads(HANDLE process, double window_seconds)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return;
        }
        const DWORD pid = GetCurrentProcessId();
        const DWORD self_tid = GetCurrentThreadId(); // the sampler thread -- exclude its own cost

        struct ThreadCpu {
            DWORD tid;
            double kernel;
            double user;
        };
        std::vector<ThreadCpu> busy;
        double process_cores = 0.0; // sum across ALL live threads, no threshold
        int live_threads = 0;

        THREADENTRY32 entry;
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self_tid) {
                    continue;
                }
                const DWORD tid = entry.th32ThreadID;
                HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
                if (th == nullptr) {
                    continue;
                }
                ++live_threads;
                FILETIME creation;
                FILETIME exit_time;
                FILETIME kernel;
                FILETIME user;
                if (GetThreadTimes(th, &creation, &exit_time, &kernel, &user)) {
                    const ULONGLONG kn = filetime_to_100ns(kernel);
                    const ULONGLONG un = filetime_to_100ns(user);
                    const auto prev = prev_thread_times.find(tid);
                    if (prev != prev_thread_times.end() && window_seconds > 0.0) {
                        const double kc =
                            static_cast<double>(kn - prev->second.first) * 1e-7 / window_seconds;
                        const double uc =
                            static_cast<double>(un - prev->second.second) * 1e-7 / window_seconds;
                        process_cores += kc + uc;
                        if (kc + uc >= 0.01) {
                            busy.push_back({tid, kc, uc});
                        }
                    }
                    prev_thread_times[tid] = {kn, un};
                }
                CloseHandle(th);
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);

        std::sort(busy.begin(), busy.end(), [](const ThreadCpu& a, const ThreadCpu& b) {
            return (a.kernel + a.user) > (b.kernel + b.user);
        });

        // process_cores counts only LIVE threads; the process's own GetProcessTimes
        // also includes threads that EXITED during the window. Print both so a gap
        // between them fingers short-lived worker threads.
        FILETIME pc, pe, pk, pu;
        double proc_total_cores = 0.0;
        if (GetProcessTimes(GetCurrentProcess(), &pc, &pe, &pk, &pu) && window_seconds > 0.0) {
            const ULONGLONG now_total = filetime_to_100ns(pk) + filetime_to_100ns(pu);
            if (prev_process_100ns != 0) {
                proc_total_cores =
                    static_cast<double>(now_total - prev_process_100ns) * 1e-7 / window_seconds;
            }
            prev_process_100ns = now_total;
        }

        std::fprintf(stderr,
                     "\n[main-sampler] pid %lu, window %.1fs -- process total %.2f cores "
                     "(GetProcessTimes %.2f), %d live threads; busiest:\n",
                     static_cast<unsigned long>(pid), window_seconds, process_cores,
                     proc_total_cores, live_threads);
        const int shown = std::min<int>(8, static_cast<int>(busy.size()));
        for (int i = 0; i < shown; ++i) {
            const ThreadCpu& t = busy[i];
            char start_symbol[320];
            thread_start_symbol(process, t.tid, start_symbol, sizeof(start_symbol));
            std::fprintf(stderr, "    tid %lu: %.2f cores (kernel %.2f, user %.2f) %s%s\n",
                         static_cast<unsigned long>(t.tid), t.kernel + t.user, t.kernel, t.user,
                         t.tid == main_tid ? "[MAIN/event-loop] " : "", start_symbol);
        }
    }

    void log_top(HANDLE process, double window_seconds)
    {
        report_threads(process, window_seconds);

        std::vector<std::pair<std::vector<DWORD64>, std::uint32_t>> entries;
        std::uint64_t total = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (histogram.empty()) {
                return;
            }
            entries.assign(histogram.begin(), histogram.end());
            total = total_samples;
            histogram.clear();
            total_samples = 0;
        }

        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        const int shown = std::min<int>(kTopStacks, static_cast<int>(entries.size()));
        std::fprintf(stderr,
                     "[main-sampler] main/event-loop thread: %llu stack samples; top %d "
                     "(innermost first; mostly Wait/MsgWait means this thread is NOT the hot one"
                     " -- see the per-thread table above):\n",
                     static_cast<unsigned long long>(total), shown);
        for (int i = 0; i < shown; ++i) {
            const std::vector<DWORD64>& stack = entries[i].first;
            const std::uint32_t count = entries[i].second;
            const double pct = total > 0 ? (100.0 * count / static_cast<double>(total)) : 0.0;
            std::fprintf(stderr, "  --- %u samples (%.1f%%) ---\n", count, pct);
            const int frames_to_print = std::min<int>(kPrintFrames, static_cast<int>(stack.size()));
            for (int f = 0; f < frames_to_print; ++f) {
                print_frame(process, stack[f]);
            }
        }
        std::fflush(stderr);
    }

    void print_frame(HANDLE process, DWORD64 addr)
    {
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 512];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;

        IMAGEHLP_MODULE64 module_info;
        module_info.SizeOfStruct = sizeof(module_info);
        const bool have_module = SymGetModuleInfo64(process, addr, &module_info) != FALSE;
        const char* module_name = have_module ? module_info.ModuleName : "?";

        DWORD64 displacement = 0;
        if (SymFromAddr(process, addr, &displacement, symbol)) {
            std::fprintf(stderr, "      %s!%s+0x%llx\n", module_name, symbol->Name,
                         static_cast<unsigned long long>(displacement));
        } else if (have_module) {
            std::fprintf(stderr, "      %s+0x%llx\n", module_name,
                         static_cast<unsigned long long>(addr - module_info.BaseOfImage));
        } else {
            std::fprintf(stderr, "      0x%llx\n", static_cast<unsigned long long>(addr));
        }
    }
};

MainThreadSampler::MainThreadSampler()
    : impl_(std::make_unique<Impl>())
{
    // A real, usable handle to the CALLING (main) thread -- GetCurrentThread() is a
    // pseudo-handle that always refers to "self", useless from the sampler thread.
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &impl_->thread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                         FALSE, 0)) {
        std::fprintf(stderr, "[main-sampler] DuplicateHandle failed (%lu); disabled\n",
                     GetLastError());
        impl_->thread = nullptr;
        return;
    }
    impl_->main_tid = GetThreadId(impl_->thread);
    impl_->sampler = std::thread([impl = impl_.get()] { impl->run(); });
    std::fprintf(stderr, "[main-sampler] sampling all threads + the main-thread stack ~200x/s\n");
}

MainThreadSampler::~MainThreadSampler()
{
    if (!impl_) {
        return;
    }
    impl_->stop.store(true, std::memory_order_relaxed);
    if (impl_->sampler.joinable()) {
        impl_->sampler.join();
    }
    if (impl_->thread != nullptr) {
        CloseHandle(impl_->thread);
    }
}

} // namespace hitsc

#else // !(HITSC_DEBUG_MAIN_SAMPLER && _WIN32) -- compile to nothing

namespace hitsc {

struct MainThreadSampler::Impl {};

MainThreadSampler::MainThreadSampler() = default;
MainThreadSampler::~MainThreadSampler() = default;

} // namespace hitsc

#endif
