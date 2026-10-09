#include "memory.h"
#include "memory_watch.h"
#include "guest.h"
#include "log.h"

#include <windows.h>

#include <cstring>

uint8_t* g_base;

extern "C" void* MmGetHostAddress(uint32_t ptr)
{
    return g_base + ptr;
}

namespace mem
{
    static Heap s_virtual4k, s_virtual64k, s_system, s_physical;
    static bool s_logPhysical = [] {
        char v[4];
        return GetEnvironmentVariableA("MCLA_LOG_PHYS", v, sizeof(v)) > 0;
    }();

    static uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

    void Heap::Init(const char* name, uint32_t base, uint32_t size, uint32_t granularity,
                    uint32_t hostOffset, bool commit)
    {
        m_name = name;
        m_base = base;
        m_size = size;
        m_granularity = granularity;
        m_hostOffset = hostOffset;
        m_commit = commit;
        m_free.clear();
        m_used.clear();
        m_free[base] = size;
    }

    uint32_t Heap::Alloc(uint32_t size, uint32_t alignment, bool topDown, bool noZero)
    {
        if (size == 0)
            return 0;
        size = AlignUp(size, m_granularity);
        alignment = alignment < m_granularity ? m_granularity : AlignUp(alignment, m_granularity);

        std::lock_guard lock(m_mutex);
        auto take = [&](uint32_t blockStart, uint32_t blockSize, uint32_t addr) {
            m_free.erase(blockStart);
            if (addr > blockStart)
                m_free[blockStart] = addr - blockStart;
            uint64_t blockEnd = uint64_t(blockStart) + blockSize, end = uint64_t(addr) + size;
            if (end < blockEnd)
                m_free[uint32_t(end)] = uint32_t(blockEnd - end);
            m_used[addr] = size;
            m_bytesUsed += size;
            return addr;
        };

        uint32_t result = 0;
        if (!topDown)
        {
            for (auto& [start, len] : m_free)
            {
                uint32_t addr = AlignUp(start, alignment);
                if (addr >= start && uint64_t(addr) + size <= uint64_t(start) + len)
                {
                    result = take(start, len, addr);
                    break;
                }
            }
        }
        else
        {
            for (auto it = m_free.rbegin(); it != m_free.rend(); ++it)
            {
                auto [start, len] = *it;
                uint64_t end = uint64_t(start) + len;
                if (end < size)
                    continue;
                uint32_t addr = uint32_t(end - size) & ~(alignment - 1);
                if (addr >= start)
                {
                    result = take(start, len, addr);
                    break;
                }
            }
        }
        if (!result)
        {
            LOGI("heap %s: out of memory (size %X align %X)", m_name, size, alignment);
            return 0;
        }
        if (m_commit)
            Commit(result, size);
        if (!noZero)
            memset(g_base + m_hostOffset + result, 0, size);
        return result;
    }

    bool Heap::AllocFixed(uint32_t addr, uint32_t size, bool* wasAllocated)
    {
        uint32_t start = addr & ~(m_granularity - 1);
        uint32_t end = AlignUp(addr + size, m_granularity);
        size = end - start;

        std::lock_guard lock(m_mutex);
        // Already inside an existing allocation: treat as a commit of reserved memory.
        auto used = m_used.upper_bound(start);
        if (used != m_used.begin())
        {
            --used;
            if (start >= used->first && uint64_t(end) <= uint64_t(used->first) + used->second)
            {
                if (wasAllocated)
                    *wasAllocated = true;
                return true;
            }
        }
        auto it = m_free.upper_bound(start);
        if (it == m_free.begin())
            return false;
        --it;
        auto [blockStart, blockSize] = *it;
        if (uint64_t(end) > uint64_t(blockStart) + blockSize)
            return false;
        m_free.erase(it);
        if (start > blockStart)
            m_free[blockStart] = start - blockStart;
        if (uint64_t(end) < uint64_t(blockStart) + blockSize)
            m_free[end] = uint32_t(uint64_t(blockStart) + blockSize - end);
        m_used[start] = size;
        m_bytesUsed += size;
        if (m_commit)
            Commit(start, size);
        memset(g_base + m_hostOffset + start, 0, size);
        if (wasAllocated)
            *wasAllocated = false;
        return true;
    }

    uint32_t Heap::Free(uint32_t addr)
    {
        std::lock_guard lock(m_mutex);
        auto it = m_used.find(addr);
        if (it == m_used.end())
            return 0;
        const uint32_t start = it->first, freed = it->second;
        m_used.erase(it);
        m_bytesUsed -= freed;

        // Insert and coalesce with neighbours.
        uint32_t size = freed;
        auto next = m_free.lower_bound(start);
        if (next != m_free.end() && uint64_t(start) + size == next->first)
        {
            size += next->second;
            next = m_free.erase(next);
        }
        if (next != m_free.begin())
        {
            auto prev = std::prev(next);
            if (uint64_t(prev->first) + prev->second == start)
            {
                prev->second += size;
                return freed;
            }
        }
        m_free[start] = size;
        return freed;
    }

    bool Heap::Query(uint32_t addr, uint32_t& allocBase, uint32_t& allocSize)
    {
        std::lock_guard lock(m_mutex);
        auto it = m_used.upper_bound(addr);
        if (it == m_used.begin())
            return false;
        --it;
        if (uint64_t(addr) >= uint64_t(it->first) + it->second)
            return false;
        allocBase = it->first;
        allocSize = it->second;
        return true;
    }

    bool Heap::QueryFree(uint32_t addr, uint32_t& regionBase, uint32_t& regionSize)
    {
        std::lock_guard lock(m_mutex);
        auto it = m_free.upper_bound(addr);
        if (it == m_free.begin())
            return false;
        --it;
        if (uint64_t(addr) >= uint64_t(it->first) + it->second)
            return false;
        regionBase = it->first;
        regionSize = it->second;
        return true;
    }

    uint64_t Heap::BytesUsed()
    {
        std::lock_guard lock(m_mutex);
        return m_bytesUsed;
    }

    void Commit(uint32_t addr, uint64_t size)
    {
        if (IsPhysical(addr))
            return; // physical views are always mapped
        uint64_t start = addr & ~0xFFFull;
        uint64_t end = (uint64_t(addr) + size + 0xFFF) & ~0xFFFull;
        if (!VirtualAlloc(g_base + start, size_t(end - start), MEM_COMMIT, PAGE_READWRITE))
            LOGI("FATAL: commit of guest %08llX+%llX failed (error %lu)", start, end - start, GetLastError());
    }

    using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

    bool Init()
    {
        // Placeholders (Windows 10 1803+) let us carve one 4 GB reservation into a
        // normal reservation for virtual memory plus three views of the same 512 MB
        // section, so physical memory aliases like it does on the console.
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        auto pVirtualAlloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kb, "VirtualAlloc2"));
        auto pMapViewOfFile3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kb, "MapViewOfFile3"));
        if (!pVirtualAlloc2 || !pMapViewOfFile3)
        {
            LOGI("FATAL: Windows 10 version 1803 or newer is required");
            return false;
        }
        HANDLE process = GetCurrentProcess();
        g_base = static_cast<uint8_t*>(pVirtualAlloc2(process, nullptr, PPC_MEMORY_SIZE,
                                                      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!g_base)
        {
            LOGI("FATAL: could not reserve 4 GB of address space (error %lu)", GetLastError());
            return false;
        }
        // Split into [0, A0000000) + three 512 MB placeholders.
        bool ok = VirtualFree(g_base, 0xA0000000, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) &&
                  VirtualFree(g_base + 0xA0000000, 0x20000000, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) &&
                  VirtualFree(g_base + 0xC0000000, 0x20000000, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        ok = ok && pVirtualAlloc2(process, g_base, 0xA0000000, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        HANDLE section = ok ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, kPhysicalSize, nullptr) : nullptr;
        char probe[4];
        bool trapE0 = GetEnvironmentVariableA("MCLA_TRAP_E0", probe, sizeof(probe)) > 0;
        for (uint64_t view : { 0xA0000000ull, 0xC0000000ull, 0xE0000000ull })
        {
            if (view == 0xE0000000ull && trapE0)
            {
                // Diagnostic: leave the E0 view unmapped so any use of it faults.
                ok = ok && pVirtualAlloc2(process, g_base + view, kPhysicalSize, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                                          PAGE_NOACCESS, nullptr, 0);
                continue;
            }
            ok = ok && section && pMapViewOfFile3(section, process, g_base + view, 0, kPhysicalSize,
                                                 MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        }
        if (!ok)
        {
            LOGI("FATAL: could not set up guest address space (error %lu)", GetLastError());
            return false;
        }

        // Layout follows the Xbox 360 (and Xenia): 0x00000000-0x3FFFFFFF 4 KB virtual,
        // 0x40000000-0x7EFFFFFF 64 KB virtual, 0x7F000000 GPU registers,
        // 0x80000000-0x9FFFFFFF executable, 0xA0000000+ physical views.
        // The low 1 MB stays unmapped so null-pointer accesses fault.
        s_virtual4k.Init("virtual4k", 0x00100000, 0x30000000 - 0x00100000, 0x1000);
        s_system.Init("system", 0x30000000, 0x10000000, 0x10);
        s_virtual64k.Init("virtual64k", 0x40000000, 0x3F000000, 0x10000);
        // Heap addresses are physical; host memory is reached through the A0000000 view.
        s_physical.Init("physical", 0x00010000, kPhysicalSize - 0x00010000, 0x1000, 0xA0000000, false);

        // GPU register window: the D3D library writes registers here directly.
        Commit(0x7F000000, 0x01000000);

        WatchInit();

        LOGI("guest memory reserved at %p", g_base);
        return true;
    }

    Heap& Virtual4K() { return s_virtual4k; }
    Heap& Virtual64K() { return s_virtual64k; }
    Heap& System() { return s_system; }

    Heap* FindHeap(uint32_t addr)
    {
        for (Heap* h : { &s_virtual4k, &s_system, &s_virtual64k })
            if (h->Contains(addr))
                return h;
        return nullptr;
    }

    uint32_t PhysicalAlloc(uint32_t size, uint32_t alignment, uint32_t pageSize)
    {
        // On hardware the 0xE0000000 (4 KB page) view is shifted by one page
        // (physical = (va & 0x1FFFFFFF) + 0x1000), and D3D hard-codes that when it
        // turns CPU pointers into GPU addresses. Windows can't map views at 4 KB
        // granularity, so 4 KB allocations are handed out from the unshifted
        // 0xA0000000 view instead; D3D then computes plain va & 0x1FFFFFFF.
        uint32_t view = pageSize >= 0x1000000 ? 0xC0000000 : 0xA0000000;
        size = AlignUp(size ? size : 1, pageSize);
        uint32_t phys = s_physical.Alloc(size, std::max(alignment, pageSize));
        if (s_logPhysical)
            LOGI("physalloc %08X-%08X (page %X)", phys, phys + size, pageSize);
        return phys ? view + phys : 0;
    }

    void PhysicalFree(uint32_t addr)
    {
        uint32_t freed = s_physical.Free(ToPhysical(addr));
        if (s_logPhysical)
            LOGI("physfree  %08X (%X bytes)%s", ToPhysical(addr), freed, freed ? "" : " NOT AN ALLOCATION");
    }

    bool PhysicalQuery(uint32_t addr, uint32_t& allocBase, uint32_t& allocSize)
    {
        if (!s_physical.Query(ToPhysical(addr), allocBase, allocSize))
            return false;
        allocBase += addr & 0xE0000000; // back into the caller's view
        return true;
    }

    uint32_t SystemAlloc(uint32_t size, uint32_t alignment)
    {
        return s_system.Alloc(size, alignment);
    }

    void SystemFree(uint32_t addr)
    {
        s_system.Free(addr);
    }
}
