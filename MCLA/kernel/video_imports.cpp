// Video (Vd*) imports. The GPU front end lives in gpu/command_processor.cpp;
// there is no renderer yet.
#include "imports.h"
#include "../gpu/command_processor.h"

#include <atomic>

namespace
{
    std::atomic<uint64_t> s_swapCount;

    void FillVideoMode(uint32_t m)
    {
        memset(g_base + m, 0, 0x30);
        Store32(m + 0x00, 1280);
        Store32(m + 0x04, 720);
        Store32(m + 0x08, 0);
        Store32(m + 0x0C, 1);
        Store32(m + 0x10, 1);
        StoreF32(m + 0x14, 60.0f);
        Store32(m + 0x18, 1);
        Store32(m + 0x1C, 0x4A);
        Store32(m + 0x20, 0x01);
    }
}

GUEST_IMPORT(VdQueryVideoMode)
{
    TRACE("VdQueryVideoMode");
    FillVideoMode(ctx.r3.u32);
}

GUEST_IMPORT(VdQueryVideoFlags)
{
    TRACE("VdQueryVideoFlags");
    ctx.r3.u64 = 3; // widescreen | >= 1024 wide
}

GUEST_IMPORT(VdGetCurrentDisplayInformation)
{
    TRACE("VdGetCurrentDisplayInformation");
    uint32_t d = ctx.r3.u32;
    memset(g_base + d, 0, 0x58);
    Store16(d + 0x00, 1280);
    Store16(d + 0x02, 720);
    Store32(d + 0x08 + 0x08, 1280); // scaler source rect x2
    Store32(d + 0x08 + 0x0C, 720);  // y2
    Store32(d + 0x08 + 0x10, 1280); // scaled output width
    Store32(d + 0x08 + 0x14, 720);
    Store32(d + 0x08 + 0x18, 1);    // vertical filter type
    Store32(d + 0x08 + 0x28, 1);    // horizontal filter type
    Store16(d + 0x40, 320);
    Store16(d + 0x42, 180);
    Store16(d + 0x44, 320);
    Store16(d + 0x46, 180);
    Store16(d + 0x48, 1280);
    Store16(d + 0x4A, 720);
    StoreF32(d + 0x4C, 60.0f);
    Store32(d + 0x50, 0);
    Store16(d + 0x56, 1280);
}

GUEST_IMPORT(VdGetCurrentDisplayGamma)
{
    TRACE("VdGetCurrentDisplayGamma");
    Store32(ctx.r3.u32, 2); // TV (BT.709)
    StoreF32(ctx.r4.u32, 2.22222233f);
}

GUEST_IMPORT(VdInitializeEngines)
{
    TRACE("VdInitializeEngines");
    ctx.r3.u64 = 1;
}

GUEST_IMPORT(VdShutdownEngines)
{
    TRACE("VdShutdownEngines");
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(VdIsHSIOTrainingSucceeded) { TRACE("VdIsHSIOTrainingSucceeded"); ctx.r3.u64 = 1; }
GUEST_IMPORT(VdRetrainEDRAM) { TRACE("VdRetrainEDRAM"); ctx.r3.u64 = 0; }
GUEST_IMPORT(VdRetrainEDRAMWorker) { TRACE("VdRetrainEDRAMWorker"); ctx.r3.u64 = 0; }
GUEST_IMPORT(VdEnableDisableClockGating) { TRACE("VdEnableDisableClockGating"); ctx.r3.u64 = 0; }
GUEST_IMPORT(VdSetDisplayMode) { TRACE("VdSetDisplayMode"); ctx.r3.u64 = 0; }
GUEST_IMPORT(VdCallGraphicsNotificationRoutines) { TRACE("VdCallGraphicsNotificationRoutines"); ctx.r3.u64 = 0; }

GUEST_IMPORT(VdPersistDisplay)
{
    TRACE("VdPersistDisplay");
    // The result must be freeable with MmFreePhysicalMemory.
    if (ctx.r4.u32)
        Store32(ctx.r4.u32, mem::PhysicalAlloc(64, 0x1000, 0x1000));
    ctx.r3.u64 = 1;
}

GUEST_IMPORT(VdInitializeScalerCommandBuffer)
{
    TRACE("VdInitializeScalerCommandBuffer");
    // dest_ptr / dest_count are args 10 and 11; fill with PM4 type-2 NOPs.
    uint32_t dest = Arg(ctx, 10), count = Arg(ctx, 11);
    for (uint32_t i = 0; i < count; i++)
        Store32(dest + i * 4, 0x80000000);
    ctx.r3.u64 = count;
}

GUEST_IMPORT(VdGetSystemCommandBuffer)
{
    TRACE("VdGetSystemCommandBuffer");
    memset(g_base + ctx.r3.u32, 0, 0x94);
    Store32(ctx.r3.u32, 0xBEEF0000);
    Store32(ctx.r4.u32, 0xBEEF0001);
}

GUEST_IMPORT(VdInitializeRingBuffer)
{
    TRACE("VdInitializeRingBuffer");
    gpu::InitializeRingBuffer(ctx.r3.u32, ctx.r4.u32);
}

GUEST_IMPORT(VdEnableRingBufferRPtrWriteBack)
{
    TRACE("VdEnableRingBufferRPtrWriteBack");
    gpu::EnableReadPointerWriteBack(ctx.r3.u32, ctx.r4.u32);
}

GUEST_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress)
{
    TRACE("VdSetSystemCommandBufferGpuIdentifierAddress");
}

GUEST_IMPORT(VdSetGraphicsInterruptCallback)
{
    TRACE("VdSetGraphicsInterruptCallback");
    gpu::SetInterruptCallback(ctx.r3.u32, ctx.r4.u32);
}

GUEST_IMPORT(VdSwap)
{
    TRACE("VdSwap");
    // (buffer in ring, fetch constant, unk, unk, unk, front buffer*, format*, color space*, width*, height*)
    uint32_t buffer = ctx.r3.u32, fetch = ctx.r4.u32;
    uint32_t frontBuffer = ctx.r8.u32 ? Load32(ctx.r8.u32) : 0;
    uint32_t width = Arg(ctx, 8) ? Load32(Arg(ctx, 8)) : 0;
    uint32_t height = Arg(ctx, 9) ? Load32(Arg(ctx, 9)) : 0;
    gpu::WriteSwapPacket(buffer, fetch, frontBuffer, width, height);
    uint64_t n = ++s_swapCount;
    static uint64_t lastDraws = 0;
    if (n == 1 || n % 300 == 0)
    {
        uint64_t draws = gpu::DrawCount();
        LOGI("VdSwap: frame %llu (%ux%u, front buffer %08X), %.0f draws/frame", n, width, height, frontBuffer,
             double(draws - lastDraws) / (n == 1 ? 1 : 300));
        lastDraws = draws;
    }
    ctx.r3.u64 = 0;
}
