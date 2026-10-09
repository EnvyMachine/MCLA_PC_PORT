// Write tracking for guest physical memory (the memory the GPU reads).
//
// The renderer keeps a GPU-side copy of physical memory and caches built from
// it (textures). Once it has copied a page it marks the page "mirrored", and a
// page a cache was built from is marked "watched": either way the page becomes
// read-only in all three host views (0xA0000000, 0xC0000000, 0xE0000000). The
// first CPU write then faults; a vectored exception handler makes the page
// writable again, clears both marks and bumps the page's generation, so the
// renderer re-uploads it before its next use, and records watched pages as
// dirty for the caches to drop what they built from them.
#pragma once

#include <cstdint>
#include <vector>

namespace mem
{
    constexpr uint32_t kWatchPageShift = 12;
    constexpr uint32_t kWatchPageSize = 1u << kWatchPageShift;
    constexpr uint32_t kWatchPageCount = 0x20000000u >> kWatchPageShift;

    void WatchInit();

    // Whether the page's current contents are in the GPU mirror.
    bool IsPageMirrored(uint32_t page);
    // Increments every time a mirrored or watched page is written.
    uint32_t PageGeneration(uint32_t page);

    // Marks pages [first, first+count) mirrored and write-protects them. Call
    // before copying their contents (a write after this point is caught).
    void MarkPagesMirrored(uint32_t first, uint32_t count);

    // Write-protects pages [first, first+count) for a cache built from their
    // contents; the first write to each reports it through TakeDirtyPages. Call
    // before reading the contents.
    void WatchPages(uint32_t first, uint32_t count);

    // Appends the watched pages written since the last call (ascending) to out.
    // Returns false (and leaves out alone) when there are none.
    bool TakeDirtyPages(std::vector<uint32_t>& out);

    // Host code is about to write [guestAddr, guestAddr+size) through a system
    // call (ReadFile), which fails instead of faulting on a protected page.
    void PrepareHostWrite(uint32_t guestAddr, uint32_t size);

    // Diagnostic: write-protects the physical page and logs (with a host
    // symbol) the first write to it.
    void TrapPageWrites(uint32_t physAddress);

    // Statistics: CPU write faults on protected pages since the last call.
    uint64_t TakeWriteFaults();
}
