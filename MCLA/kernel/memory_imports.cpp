// Virtual/physical memory and pool allocation imports.
#include "imports.h"

namespace
{
    constexpr uint32_t kMemCommit = 0x1000;
    constexpr uint32_t kMemReserve = 0x2000;
    constexpr uint32_t kMemDecommit = 0x4000;
    constexpr uint32_t kMemRelease = 0x8000;
    constexpr uint32_t kMemFree = 0x10000;
    constexpr uint32_t kMemPrivate = 0x20000;
    constexpr uint32_t kMemReset = 0x80000;
    constexpr uint32_t kMemTopDown = 0x100000;
    constexpr uint32_t kMemNoZero = 0x800000;
    constexpr uint32_t kMemLargePages = 0x20000000;
    constexpr uint32_t kMem16MBPages = 0x80000000;
    constexpr uint32_t kPageReadWrite = 0x04;
    constexpr uint32_t kPageNoAccess = 0x01;

    uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

    bool IsVirtualHeap(mem::Heap* h) { return h == &mem::Virtual4K() || h == &mem::Virtual64K(); }
}

GUEST_IMPORT(NtAllocateVirtualMemory)
{
    TRACE("NtAllocateVirtualMemory");
    uint32_t pBase = ctx.r3.u32, pSize = ctx.r4.u32, type = ctx.r5.u32;
    if (!pBase || !pSize || !Load32(pSize) || !(type & (kMemCommit | kMemReserve | kMemReset)))
    {
        ctx.r3.u64 = xstatus::InvalidParameter;
        return;
    }
    if (type & kMemReset)
    {
        ctx.r3.u64 = xstatus::Success;
        return;
    }

    uint32_t requested = Load32(pBase);
    uint32_t size = Load32(pSize);
    if (int32_t(size) < 0) // some titles pass negative sizes
        size = uint32_t(-int32_t(size));

    uint32_t address = 0;
    if (requested)
    {
        mem::Heap* heap = mem::FindHeap(requested);
        if (!IsVirtualHeap(heap))
        {
            ctx.r3.u64 = xstatus::InvalidParameter;
            return;
        }
        uint32_t page = heap->Granularity();
        uint32_t start = requested & ~(page - 1);
        size = AlignUp(size + (requested - start), page);
        if (heap->AllocFixed(start, size))
            address = start;
    }
    else
    {
        bool large = type & kMemLargePages;
        mem::Heap& heap = large ? mem::Virtual64K() : mem::Virtual4K();
        size = AlignUp(size, 0x10000);
        address = heap.Alloc(size, large ? 0x10000 : 0x1000, type & kMemTopDown, type & kMemNoZero);
    }

    if (!address)
    {
        LOGI("NtAllocateVirtualMemory: failed (base %08X size %X type %X)", requested, size, type);
        ctx.r3.u64 = xstatus::NoMemory;
        return;
    }
    Store32(pBase, address);
    Store32(pSize, size);
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(NtFreeVirtualMemory)
{
    TRACE("NtFreeVirtualMemory");
    uint32_t pBase = ctx.r3.u32, pSize = ctx.r4.u32, type = ctx.r5.u32;
    uint32_t addr = pBase ? Load32(pBase) : 0;
    mem::Heap* heap = mem::FindHeap(addr);
    if (!addr || !IsVirtualHeap(heap))
    {
        ctx.r3.u64 = xstatus::InvalidParameter;
        return;
    }
    if (type == kMemDecommit)
    {
        // Pages stay committed on the host; decommitted memory reads back as zero.
        uint32_t page = heap->Granularity();
        uint32_t size = AlignUp(pSize ? Load32(pSize) : 0, page);
        uint32_t start = addr & ~(page - 1);
        uint32_t allocBase, allocSize;
        if (size && heap->Query(addr, allocBase, allocSize))
            memset(g_base + start, 0, std::min<uint64_t>(size, uint64_t(allocBase) + allocSize - start));
        ctx.r3.u64 = xstatus::Success;
        return;
    }
    uint32_t freed = heap->Free(addr);
    if (!freed)
    {
        ctx.r3.u64 = xstatus::Unsuccessful;
        return;
    }
    if (pSize)
        Store32(pSize, freed);
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(NtQueryVirtualMemory)
{
    TRACE("NtQueryVirtualMemory");
    uint32_t addr = ctx.r3.u32, info = ctx.r4.u32;
    if (info && mem::IsPhysical(addr))
    {
        uint32_t allocBase, allocSize;
        bool used = mem::PhysicalQuery(addr, allocBase, allocSize);
        uint32_t start = addr & ~0xFFFu;
        Store32(info + 0x00, start);
        Store32(info + 0x04, used ? allocBase : 0);
        Store32(info + 0x08, used ? kPageReadWrite : 0);
        Store32(info + 0x0C, used ? allocBase + allocSize - start : 0x1000);
        Store32(info + 0x10, used ? kMemCommit : kMemFree);
        Store32(info + 0x14, used ? kPageReadWrite : kPageNoAccess);
        Store32(info + 0x18, used ? kMemPrivate : 0);
        ctx.r3.u64 = xstatus::Success;
        return;
    }
    mem::Heap* heap = mem::FindHeap(addr);
    if (!heap || !info)
    {
        ctx.r3.u64 = xstatus::InvalidParameter;
        return;
    }
    // X_MEMORY_BASIC_INFORMATION: base, allocation_base, allocation_protect,
    // region_size, state, protect, type
    uint32_t page = std::max(heap->Granularity(), 0x1000u);
    uint32_t start = addr & ~(page - 1);
    uint32_t allocBase, allocSize;
    if (heap->Query(addr, allocBase, allocSize))
    {
        Store32(info + 0x00, start);
        Store32(info + 0x04, allocBase);
        Store32(info + 0x08, kPageReadWrite);
        Store32(info + 0x0C, allocBase + allocSize - start);
        Store32(info + 0x10, kMemCommit);
        Store32(info + 0x14, kPageReadWrite);
        Store32(info + 0x18, kMemPrivate);
    }
    else
    {
        uint32_t freeBase = start, freeSize = page;
        heap->QueryFree(addr, freeBase, freeSize);
        Store32(info + 0x00, start);
        Store32(info + 0x04, 0);
        Store32(info + 0x08, 0);
        Store32(info + 0x0C, freeBase + freeSize - start);
        Store32(info + 0x10, kMemFree);
        Store32(info + 0x14, kPageNoAccess);
        Store32(info + 0x18, 0);
    }
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(MmAllocatePhysicalMemoryEx)
{
    TRACE("MmAllocatePhysicalMemoryEx");
    uint32_t size = ctx.r4.u32, protect = ctx.r5.u32, alignment = ctx.r8.u32;
    uint32_t page = (protect & kMemLargePages) ? 0x10000 : (protect & kMem16MBPages) ? 0x1000000 : 0x1000;
    uint32_t address = mem::PhysicalAlloc(size, AlignUp(alignment, page), page);
    if (!address)
        LOGI("MmAllocatePhysicalMemoryEx: failed (size %X protect %X align %X)", size, protect, alignment);
    else if (g_traceImports)
        LOGI("MmAllocatePhysicalMemoryEx(size %X, protect %X, align %X) = %08X", size, protect, alignment, address);
    ctx.r3.u64 = address;
}

GUEST_IMPORT(MmFreePhysicalMemory)
{
    TRACE("MmFreePhysicalMemory");
    if (mem::IsPhysical(ctx.r4.u32))
        mem::PhysicalFree(ctx.r4.u32);
}

GUEST_IMPORT(MmGetPhysicalAddress)
{
    TRACE("MmGetPhysicalAddress");
    ctx.r3.u64 = mem::ToPhysical(ctx.r3.u32);
}

GUEST_IMPORT(MmQueryAllocationSize)
{
    TRACE("MmQueryAllocationSize");
    uint32_t addr = ctx.r3.u32, allocBase = 0, allocSize = 0;
    bool found = false;
    if (mem::IsPhysical(addr))
        found = mem::PhysicalQuery(addr, allocBase, allocSize);
    else if (mem::Heap* heap = mem::FindHeap(addr))
        found = heap->Query(addr, allocBase, allocSize);
    static const bool logPhysical = [] {
        char v[4];
        return GetEnvironmentVariableA("MCLA_LOG_PHYS", v, sizeof(v)) > 0;
    }();
    // The size from the address to the end of its allocation. RAGE clears texture
    // memory with memset(address, 0, MmQueryAllocationSize(address)) for addresses
    // inside larger allocations (e.g. the second half of a pool), so returning the
    // whole allocation's size overruns into whatever follows it.
    uint32_t remaining = 0;
    if (found)
    {
        uint32_t offset = (addr - allocBase) & ~0xFFFu; // both in the caller's view
        remaining = offset < allocSize ? allocSize - offset : 0;
    }
    if (g_traceImports || logPhysical)
        LOGI("MmQueryAllocationSize(%08X) = %X (allocation %08X, %X bytes)", addr, remaining, allocBase, allocSize);
    ctx.r3.u64 = remaining;
}

GUEST_IMPORT(MmQueryAddressProtect)
{
    TRACE("MmQueryAddressProtect");
    uint32_t addr = ctx.r3.u32, allocBase, allocSize;
    mem::Heap* heap = mem::FindHeap(addr);
    bool allocated = (mem::IsPhysical(addr) && mem::PhysicalQuery(addr, allocBase, allocSize)) ||
                     (heap && heap->Query(addr, allocBase, allocSize)) ||
                     (addr >= g_xex.imageBase && addr < g_xex.imageBase + g_xex.imageSize);
    ctx.r3.u64 = allocated ? kPageReadWrite : 0;
}

GUEST_IMPORT(ExAllocatePoolTypeWithTag)
{
    TRACE("ExAllocatePoolTypeWithTag");
    ctx.r3.u64 = mem::SystemAlloc(ctx.r3.u32, 16);
}

GUEST_IMPORT(ExFreePool)
{
    TRACE("ExFreePool");
    mem::SystemFree(ctx.r3.u32);
}

GUEST_IMPORT(XamAlloc)
{
    TRACE("XamAlloc");
    uint32_t size = ctx.r4.u32, out = ctx.r5.u32;
    uint32_t addr = mem::SystemAlloc(size ? size : 1, 16);
    if (out)
        Store32(out, addr);
    ctx.r3.u64 = addr ? 0 : 0x8007000E; // E_OUTOFMEMORY
}

GUEST_IMPORT(XamFree)
{
    TRACE("XamFree");
    mem::SystemFree(ctx.r3.u32);
    ctx.r3.u64 = 0;
}
