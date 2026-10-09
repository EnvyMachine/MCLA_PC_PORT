#include "memory_watch.h"
#include "guest.h"
#include "log.h"
#include "memory.h"

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <memory>

namespace mem
{
    namespace
    {
        constexpr uint32_t kViews[] = { 0xA0000000, 0xC0000000, 0xE0000000 };

        // Page state bits; a page is write-protected while any is set.
        constexpr uint8_t kMirrored = 1, kWatched = 2;

        std::unique_ptr<std::atomic<uint8_t>[]> s_state;
        std::unique_ptr<uint8_t[]> s_trapped; // diagnostic traps (TrapPageWrites)
        std::unique_ptr<std::atomic<uint32_t>[]> s_generation;
        // Watched pages written since the last TakeDirtyPages, one bit each.
        // Guarded by s_lock; a fixed array so the exception handler never allocates.
        std::unique_ptr<uint64_t[]> s_dirty;
        std::atomic<bool> s_anyDirty{ false };
        SRWLOCK s_lock = SRWLOCK_INIT;
        std::atomic<uint64_t> s_faults{ 0 }; // write faults on watched/mirrored pages

        void Protect(uint32_t firstPage, uint32_t count, DWORD protection)
        {
            for (uint32_t view : kViews)
            {
                DWORD old;
                VirtualProtect(g_base + view + (size_t(firstPage) << kWatchPageShift), size_t(count) << kWatchPageShift,
                               protection, &old);
            }
        }

        // Caller holds s_lock exclusively.
        void Unprotect(uint32_t page)
        {
            uint8_t state = s_state[page].load(std::memory_order_relaxed);
            if (!state)
                return;
            Protect(page, 1, PAGE_READWRITE);
            s_state[page].store(0, std::memory_order_relaxed);
            s_generation[page].fetch_add(1, std::memory_order_release);
            if (state & kWatched)
            {
                s_dirty[page >> 6] |= 1ull << (page & 63);
                s_anyDirty.store(true, std::memory_order_release);
            }
        }

        LONG CALLBACK Handler(EXCEPTION_POINTERS* ep)
        {
            const EXCEPTION_RECORD* er = ep->ExceptionRecord;
            if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || er->ExceptionInformation[0] != 1)
                return EXCEPTION_CONTINUE_SEARCH;
            uintptr_t address = er->ExceptionInformation[1];
            uintptr_t start = uintptr_t(g_base) + 0xA0000000;
            if (address < start || address >= uintptr_t(g_base) + 0x100000000ull)
                return EXCEPTION_CONTINUE_SEARCH;
            uint32_t page = uint32_t((address - uintptr_t(g_base)) & 0x1FFFFFFF) >> kWatchPageShift;
            AcquireSRWLockExclusive(&s_lock);
            if (s_trapped[page])
            {
                s_trapped[page] = 0;
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
                auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 255;
                DWORD64 displacement = 0;
                bool named = SymFromAddr(GetCurrentProcess(), ep->ContextRecord->Rip, &displacement, symbol);
                LOGI("watch: write to trapped page %05X (guest %08llX) from %s+0x%llX", page,
                     (unsigned long long)(address - uintptr_t(g_base)), named ? symbol->Name : "?",
                     (unsigned long long)displacement);
                // Callers (recompiled functions are named after their guest address).
                CONTEXT context = *ep->ContextRecord;
                STACKFRAME64 frame{};
                frame.AddrPC.Offset = context.Rip;
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = context.Rbp;
                frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = context.Rsp;
                frame.AddrStack.Mode = AddrModeFlat;
                for (int i = 0; i < 8; i++)
                {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &context,
                                     nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                        !frame.AddrPC.Offset)
                        break;
                    if (SymFromAddr(GetCurrentProcess(), frame.AddrPC.Offset, &displacement, symbol))
                        LOGI("watch:   #%d %s+0x%llX", i, symbol->Name, (unsigned long long)displacement);
                }
                if (!s_state[page].load(std::memory_order_relaxed))
                    Protect(page, 1, PAGE_READWRITE);
            }
            else if (!s_state[page].load(std::memory_order_relaxed))
            {
                // Not ours, unless another thread's fault unprotected it just now.
                MEMORY_BASIC_INFORMATION info;
                bool writable = VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) &&
                                (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
                ReleaseSRWLockExclusive(&s_lock);
                return writable ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
            }
            Unprotect(page);
            ReleaseSRWLockExclusive(&s_lock);
            s_faults.fetch_add(1, std::memory_order_relaxed);
            // Writable now: retry.
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        void Mark(uint32_t first, uint32_t count, uint8_t bit)
        {
            if (first >= kWatchPageCount)
                return;
            count = std::min(count, kWatchPageCount - first);
            AcquireSRWLockExclusive(&s_lock);
            Protect(first, count, PAGE_READONLY);
            for (uint32_t i = 0; i < count; i++)
                s_state[first + i].fetch_or(bit, std::memory_order_release);
            ReleaseSRWLockExclusive(&s_lock);
        }
    }

    void WatchInit()
    {
        s_state.reset(new std::atomic<uint8_t>[kWatchPageCount]());
        s_generation.reset(new std::atomic<uint32_t>[kWatchPageCount]());
        s_trapped.reset(new uint8_t[kWatchPageCount]());
        s_dirty.reset(new uint64_t[kWatchPageCount / 64]());
        // First in the chain, ahead of the crash reporter.
        AddVectoredExceptionHandler(1, Handler);
    }

    bool IsPageMirrored(uint32_t page) { return (s_state[page].load(std::memory_order_acquire) & kMirrored) != 0; }

    uint64_t TakeWriteFaults() { return s_faults.exchange(0, std::memory_order_relaxed); }
    uint32_t PageGeneration(uint32_t page) { return s_generation[page].load(std::memory_order_acquire); }

    void MarkPagesMirrored(uint32_t first, uint32_t count) { Mark(first, count, kMirrored); }
    void WatchPages(uint32_t first, uint32_t count) { Mark(first, count, kWatched); }

    bool TakeDirtyPages(std::vector<uint32_t>& out)
    {
        if (!s_anyDirty.load(std::memory_order_acquire))
            return false;
        AcquireSRWLockExclusive(&s_lock);
        s_anyDirty.store(false, std::memory_order_relaxed);
        for (uint32_t word = 0; word < kWatchPageCount / 64; word++)
        {
            for (uint64_t bits = s_dirty[word]; bits; bits &= bits - 1)
                out.push_back(word * 64 + uint32_t(__builtin_ctzll(bits)));
            s_dirty[word] = 0;
        }
        ReleaseSRWLockExclusive(&s_lock);
        return true;
    }

    void TrapPageWrites(uint32_t physAddress)
    {
        uint32_t page = (physAddress & 0x1FFFFFFF) >> kWatchPageShift;
        AcquireSRWLockExclusive(&s_lock);
        s_trapped[page] = 1;
        Protect(page, 1, PAGE_READONLY);
        ReleaseSRWLockExclusive(&s_lock);
    }

    void PrepareHostWrite(uint32_t guestAddr, uint32_t size)
    {
        if (!IsPhysical(guestAddr) || !size || !s_state)
            return;
        uint32_t first = ToPhysical(guestAddr) >> kWatchPageShift;
        uint32_t last = (ToPhysical(guestAddr) + size - 1) >> kWatchPageShift;
        AcquireSRWLockExclusive(&s_lock);
        for (uint32_t page = first; page <= last && page < kWatchPageCount; page++)
            Unprotect(page);
        ReleaseSRWLockExclusive(&s_lock);
    }
}
