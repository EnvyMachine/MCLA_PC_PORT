// Crash reporting: logs the faulting guest address, the last kernel import the
// thread called and a symbolised host stack. Recompiled functions are named
// sub_<guest address>, so the host stack doubles as the guest call stack.
#include "guest.h"
#include "loader.h"
#include "log.h"
#include "thread.h"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace crash
{
    static std::mutex s_lock;

    // Set around guarded reads (__try) whose faults are expected and handled.
    thread_local bool t_probing = false;

    static void LogStack(CONTEXT* context, HANDLE threadHandle = GetCurrentThread(), int maxFrames = 48)
    {
        HANDLE process = GetCurrentProcess();
        CONTEXT ctx = *context;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;

        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        for (int i = 0; i < maxFrames; i++)
        {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, threadHandle, &frame, &ctx, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
                break;
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            IMAGEHLP_LINE64 line{ sizeof(line) };
            DWORD lineDisplacement = 0;
            // Recompiled functions: the source line carries the PPC instruction as a comment.
            char where[160] = "";
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line))
            {
                const char* file = strrchr(line.FileName, '\\');
                snprintf(where, sizeof(where), " (%s:%lu)", file ? file + 1 : line.FileName, line.LineNumber);
            }
            if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
                LogWrite("  #%02d %s+0x%llX%s", i, symbol->Name, displacement, where);
            else
                LogWrite("  #%02d %016llX", i, frame.AddrPC.Offset);
        }
    }

    static LONG CALLBACK Handler(EXCEPTION_POINTERS* ep)
    {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (t_probing)
            return EXCEPTION_CONTINUE_SEARCH;
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_BREAKPOINT:
            break;
        default:
            return EXCEPTION_CONTINUE_SEARCH; // C++ exceptions (thread exit), debug output, ...
        }

        std::lock_guard lock(s_lock);
        GuestThread* t = thread::Current();
        LogWrite("==== CRASH: exception %08lX at %p on %s (last import: %s)", code,
                 ep->ExceptionRecord->ExceptionAddress, t ? t->name.c_str() : "host thread", LastImportCall());
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        {
            uint64_t addr = ep->ExceptionRecord->ExceptionInformation[1];
            const char* op = ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "read";
            if (addr >= uint64_t(g_base) && addr < uint64_t(g_base) + PPC_MEMORY_SIZE)
                LogWrite("  %s of guest address %08llX", op, addr - uint64_t(g_base));
            else
                LogWrite("  %s of host address %016llX", op, addr);
        }
        LogStack(ep->ContextRecord);
        LogFlush();
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Suspends each guest thread in turn and logs where it is (hang diagnosis).
    void DumpAllThreads()
    {
        std::lock_guard lock(s_lock);
        LogWrite("==== thread dump ====");
        // D3D device GPU-progress fields (device pointer published in VdGlobalDevice).
        if (uint32_t dev = Load32(g_xex.vdGlobalDevice))
        {
            uint32_t p2a90 = Load32(dev + 0x2A90);
            LogWrite("d3d device %08X: [2A88]=%08X [2A90]=%08X (*=%08X) [2A94]=%08X [2A9C]=%08X  CP_RB_WPTR=%08X",
                     dev, Load32(dev + 0x2A88), p2a90, p2a90 ? Load32(p2a90) : 0, Load32(dev + 0x2A94),
                     Load32(dev + 0x2A9C), Load32(0x7FC80714));
        }
        else
            LogWrite("VdGlobalDevice not set; CP_RB_WPTR=%08X", Load32(0x7FC80714));
        // MCLA's XAudio engine object (global 0x828CD9BC): participant count and per-CPU slots.
        if (uint32_t engine = Load32(0x828CD9BC))
            LogWrite("xaudio engine %08X: [130]=%u per-cpu %08X %08X %08X %08X %08X %08X", engine, Load32(engine + 0x130),
                     Load32(engine + 0x134), Load32(engine + 0x138), Load32(engine + 0x13C), Load32(engine + 0x140),
                     Load32(engine + 0x144), Load32(engine + 0x148));
        for (GuestThread* t : thread::All())
        {
            HANDLE h = static_cast<HANDLE>(t->hostHandle);
            if (!h || h == GetCurrentThread())
                continue;
            if (SuspendThread(h) == DWORD(-1))
                continue;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &ctx))
            {
                LogWrite("-- %s (guest id %u, cpu %u), last import: %s", t->name.c_str(), t->id, Load8(t->pcr + 0x10C),
                         t->lastImport);
                LogStack(&ctx, h, 14);
            }
            ResumeThread(h);
        }
        LogFlush();
    }

    // ---- diagnostic hardware write watchpoints ----------------------------------

    static std::atomic<uint32_t> s_watchAddr, s_watchHits, s_watchMax;
    static std::atomic<bool> s_watchReads;

    static LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* ep)
    {
        if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 1))
            return EXCEPTION_CONTINUE_SEARCH;
        CONTEXT* c = ep->ContextRecord;
        c->Dr6 = 0;
        // Access watches skip hits from the function named in MCLA_WATCH_IGNORE (e.g. the writer).
        static char ignore[64] = {};
        static bool ignoreLoaded = GetEnvironmentVariableA("MCLA_WATCH_IGNORE", ignore, sizeof(ignore)) > 0;
        (void)ignoreLoaded;
        if (ignore[0])
        {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 128];
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 127;
            DWORD64 displacement;
            std::lock_guard lock(s_lock);
            if (SymFromAddr(GetCurrentProcess(), c->Rip, &displacement, symbol) && strstr(symbol->Name, ignore))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
        uint32_t n = s_watchHits++;
        if (n < s_watchMax)
        {
            std::lock_guard lock(s_lock);
            uint32_t addr = s_watchAddr;
            GuestThread* t = thread::Current();
            LogWrite("watch: %s #%u at %08X: %08X (%g) on %s", s_watchReads ? "access" : "write", n, addr, Load32(addr),
                     LoadF32(addr), t ? t->name.c_str() : "host thread");
            LogStack(c, GetCurrentThread(), 12);
        }
        else
            c->Dr7 &= ~1ull;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Logs the writer (host stack = guest call stack) of the next maxHits writes to the
    // 4 bytes at guestAddr, in every guest thread that exists now.
    void WatchWrites(uint32_t guestAddr, uint32_t maxHits, bool reads)
    {
        static bool installed = [] { return AddVectoredExceptionHandler(1, WatchHandler) != nullptr; }();
        (void)installed;
        s_watchAddr = guestAddr & ~3u;
        s_watchHits = 0;
        s_watchMax = maxHits;
        s_watchReads = reads;
        LogWrite("watch: watching %s %08X", reads ? "accesses to" : "writes to", guestAddr & ~3u);
        // Another thread sets the debug registers so the caller can be suspended too.
        std::thread([guestAddr, reads] {
            int set = 0, failed = 0;
            for (GuestThread* t : thread::All())
            {
                HANDLE h = static_cast<HANDLE>(t->hostHandle);
                if (!h || SuspendThread(h) == DWORD(-1))
                {
                    failed++;
                    continue;
                }
                CONTEXT c{};
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(h, &c))
                {
                    c.Dr0 = uint64_t(g_base) + (guestAddr & ~3u);
                    // L0, 4 bytes, write (RW=01) or read/write (RW=11)
                    c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | ((reads ? 3ull : 1ull) << 16) | (3ull << 18);
                    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    (SetThreadContext(h, &c) ? set : failed)++;
                }
                else
                    failed++;
                ResumeThread(h);
            }
            LogWrite("watch: debug registers set on %d threads (%d failed)", set, failed);
        }).join();
    }

    void Install()
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        AddVectoredExceptionHandler(1, Handler);
        // MCLA_WATCH_ADDR=<guest address>@<seconds>[,<hits>]: watch writes to it from then on.
        if (char v[48]; GetEnvironmentVariableA("MCLA_WATCH_ADDR", v, sizeof(v)) > 0)
        {
            unsigned addr = 0, hits = 20;
            double seconds = 0;
            if (sscanf(v, "%x@%lf,%u", &addr, &seconds, &hits) >= 2)
                std::thread([addr, seconds, hits] {
                    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
                    WatchWrites(addr, hits, false);
                }).detach();
        }
    }
}
