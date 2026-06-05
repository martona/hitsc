#pragma once

#include <memory>

// In-app sampling profiler for the GUI/main thread.
//
// Uncomment to enable. When on, constructing a MainThreadSampler on the thread you
// want to profile spawns a side thread that, ~200x/second, suspends that thread,
// walks its stack, resumes it, and (every couple of seconds) logs the hottest
// stacks to stderr with sample counts. It catches the main thread MID-BURN without
// any manual capture -- the side thread samples continuously while you move the
// mouse, so you never have to click anything to freeze the target.
//
// Windows-only; a no-op (empty ctor/dtor) when the macro is undefined or off
// Windows. Auto-links dbghelp.lib via #pragma. Symbol names resolve for hitsc.exe
// (its PDB) and as module+offset for Qt/system DLLs without symbols.
//
//#define HITSC_DEBUG_MAIN_SAMPLER 1

namespace hitsc {

class MainThreadSampler {
public:
    // Construct ON the thread to profile (it captures the *calling* thread).
    MainThreadSampler();
    ~MainThreadSampler();

    MainThreadSampler(const MainThreadSampler&) = delete;
    MainThreadSampler& operator=(const MainThreadSampler&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hitsc
