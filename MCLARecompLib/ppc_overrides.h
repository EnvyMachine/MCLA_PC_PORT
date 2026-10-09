// Force-included into the recompiled code (before ppc_context.h).
// Indirect calls go through a checked lookup so a call to a guest address with
// no recompiled function is reported instead of jumping to null.
#pragma once

#include <cstdint>
#include <intrin.h>

// mftb: the game converts timebase ticks to seconds with a hardcoded 1/49875000,
// so scale the host TSC to the Xbox 360's timebase frequency (kern::TimebaseFrequency).
extern uint64_t g_tscOrigin;
extern uint64_t g_tscToTimebase; // 32.32 fixed point
inline uint64_t PPCQueryTimebase()
{
    return uint64_t((unsigned __int128)(__rdtsc() - g_tscOrigin) * g_tscToTimebase >> 32);
}
#define PPC_QUERY_TIMEBASE() PPCQueryTimebase()

struct PPCContext;
void PPCMissingFunction(PPCContext& ctx, uint8_t* base, uint32_t target);

#define PPC_CALL_INDIRECT_FUNC(x)                                                              \
    do                                                                                         \
    {                                                                                          \
        uint32_t ppcTarget_ = uint32_t(x);                                                     \
        PPCFunc* ppcFn_ = (ppcTarget_ - uint32_t(PPC_CODE_BASE)) < uint32_t(PPC_CODE_SIZE)     \
            ? PPC_LOOKUP_FUNC(base, ppcTarget_) : nullptr;                                     \
        if (ppcFn_) [[likely]]                                                                 \
            ppcFn_(ctx, base);                                                                 \
        else                                                                                   \
            PPCMissingFunction(ctx, base, ppcTarget_);                                         \
    } while (0)
