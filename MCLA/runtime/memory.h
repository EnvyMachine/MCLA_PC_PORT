// Guest address space: a 4 GB host reservation with heaps mirroring the Xbox 360
// layout (see notes/PROGRESS.md). Pages are committed when allocated, so stray
// accesses to unallocated guest memory fault instead of silently succeeding.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>

namespace mem
{
    class Heap
    {
    public:
        // hostOffset: host memory for heap address A is g_base + hostOffset + A.
        // commit: whether allocations need committing (false for mapped views).
        void Init(const char* name, uint32_t base, uint32_t size, uint32_t granularity,
                  uint32_t hostOffset = 0, bool commit = true);

        // Returns 0 on failure. Memory is committed and zeroed unless noZero.
        uint32_t Alloc(uint32_t size, uint32_t alignment, bool topDown = false, bool noZero = false);
        // Claims [addr, addr+size). Succeeds if the range is free, or already
        // lies inside one existing allocation (commit-after-reserve).
        bool AllocFixed(uint32_t addr, uint32_t size, bool* wasAllocated = nullptr);
        // Frees the allocation starting at addr; returns its size (0 if none).
        uint32_t Free(uint32_t addr);
        // Finds the allocation containing addr.
        bool Query(uint32_t addr, uint32_t& allocBase, uint32_t& allocSize);
        // Returns the free region containing addr (for MEM_FREE queries).
        bool QueryFree(uint32_t addr, uint32_t& regionBase, uint32_t& regionSize);

        bool Contains(uint32_t addr) const { return addr >= m_base && uint64_t(addr) < uint64_t(m_base) + m_size; }
        uint32_t Granularity() const { return m_granularity; }
        uint32_t Base() const { return m_base; }
        uint64_t BytesUsed();

    private:
        const char* m_name = "";
        uint32_t m_base = 0, m_size = 0, m_granularity = 0, m_hostOffset = 0;
        bool m_commit = true;
        std::mutex m_mutex;
        std::map<uint32_t, uint32_t> m_free; // start -> size
        std::map<uint32_t, uint32_t> m_used; // start -> size
        uint64_t m_bytesUsed = 0;
    };

    bool Init();
    void Commit(uint32_t addr, uint64_t size);

    Heap& Virtual4K();   // NtAllocateVirtualMemory, 4 KB pages
    Heap& Virtual64K();  // NtAllocateVirtualMemory with MEM_LARGE_PAGES
    Heap& System();      // runtime-owned kernel objects, thread blocks, pools
    // Virtual heaps only (physical memory is handled below).
    Heap* FindHeap(uint32_t addr);

    // Physical memory: 512 MB mapped three times, as on the console, at
    // 0xA0000000 (64 KB pages), 0xC0000000 (16 MB pages) and 0xE0000000 (4 KB pages).
    // physical address = virtual & 0x1FFFFFFF.
    constexpr uint32_t kPhysicalSize = 0x20000000;
    inline bool IsPhysical(uint32_t addr) { return addr >= 0xA0000000; }
    inline uint32_t ToPhysical(uint32_t addr) { return addr & 0x1FFFFFFF; }
    // Allocates physical memory and returns its address in the view for pageSize.
    uint32_t PhysicalAlloc(uint32_t size, uint32_t alignment, uint32_t pageSize);
    void PhysicalFree(uint32_t addr);
    bool PhysicalQuery(uint32_t addr, uint32_t& allocBase, uint32_t& allocSize);

    // Small allocations for runtime-owned guest structures.
    uint32_t SystemAlloc(uint32_t size, uint32_t alignment = 16);
    void SystemFree(uint32_t addr);
}
