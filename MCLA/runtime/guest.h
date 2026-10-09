// Core helpers shared by the runtime and the kernel import implementations.
#pragma once

#include <cstdint>
#include <cstring>

#include <ppc_context.h>
#include <xbox.h>

// Base of the 4 GB block that stands in for the Xbox 360 address space.
// Guest address A lives at g_base + A.
extern uint8_t* g_base;

template <typename T = uint8_t>
inline T* GuestPtr(uint32_t addr)
{
    return addr ? reinterpret_cast<T*>(g_base + addr) : nullptr;
}

inline uint32_t GuestAddr(const void* host)
{
    return host ? uint32_t(static_cast<const uint8_t*>(host) - g_base) : 0;
}

// Big-endian accessors for guest memory.
inline uint8_t Load8(uint32_t a) { return g_base[a]; }
inline uint16_t Load16(uint32_t a) { uint16_t v; memcpy(&v, g_base + a, 2); return __builtin_bswap16(v); }
inline uint32_t Load32(uint32_t a) { uint32_t v; memcpy(&v, g_base + a, 4); return __builtin_bswap32(v); }
inline uint64_t Load64(uint32_t a) { uint64_t v; memcpy(&v, g_base + a, 8); return __builtin_bswap64(v); }
inline void Store8(uint32_t a, uint8_t v) { g_base[a] = v; }
inline void Store16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(g_base + a, &v, 2); }
inline void Store32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(g_base + a, &v, 4); }
inline void Store64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(g_base + a, &v, 8); }

inline float LoadF32(uint32_t a) { uint32_t u = Load32(a); float f; memcpy(&f, &u, 4); return f; }
inline void StoreF32(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); Store32(a, u); }

// Import arguments: the first eight live in r3-r10, the rest on the caller's stack
// in 8-byte slots starting at r1 + 0x54 (low word of each slot).
inline uint32_t Arg(PPCContext& ctx, int index)
{
    switch (index)
    {
    case 0: return ctx.r3.u32;
    case 1: return ctx.r4.u32;
    case 2: return ctx.r5.u32;
    case 3: return ctx.r6.u32;
    case 4: return ctx.r7.u32;
    case 5: return ctx.r8.u32;
    case 6: return ctx.r9.u32;
    case 7: return ctx.r10.u32;
    default: return Load32(ctx.r1.u32 + 0x54 + (index - 8) * 8);
    }
}

// Defines the host implementation of an Xbox kernel/XAM import. The recompiled
// code calls these through the __imp__<Name> symbols.
#define GUEST_IMPORT(name) PPC_FUNC(__imp__##name)

// Calls a guest function (recompiled) at a guest address with the given context.
void CallGuest(PPCContext& ctx, uint32_t address);

namespace xstatus
{
    constexpr uint32_t Success = 0x00000000;
    constexpr uint32_t Abandoned = 0x00000080;
    constexpr uint32_t UserApc = 0x000000C0;
    constexpr uint32_t Alerted = 0x00000101;
    constexpr uint32_t Timeout = 0x00000102;
    constexpr uint32_t Pending = 0x00000103;
    constexpr uint32_t BufferOverflow = 0x80000005;
    constexpr uint32_t NoMoreFiles = 0x80000006;
    constexpr uint32_t Unsuccessful = 0xC0000001;
    constexpr uint32_t NotImplemented = 0xC0000002;
    constexpr uint32_t InfoLengthMismatch = 0xC0000004;
    constexpr uint32_t InvalidHandle = 0xC0000008;
    constexpr uint32_t InvalidParameter = 0xC000000D;
    constexpr uint32_t NoSuchFile = 0xC000000F;
    constexpr uint32_t EndOfFile = 0xC0000011;
    constexpr uint32_t NoMemory = 0xC0000017;
    constexpr uint32_t AlreadyCommitted = 0xC0000021;
    constexpr uint32_t AccessDenied = 0xC0000022;
    constexpr uint32_t BufferTooSmall = 0xC0000023;
    constexpr uint32_t ObjectTypeMismatch = 0xC0000024;
    constexpr uint32_t ObjectNameInvalid = 0xC0000033;
    constexpr uint32_t ObjectNameNotFound = 0xC0000034;
    constexpr uint32_t ObjectNameCollision = 0xC0000035;
    constexpr uint32_t ObjectPathNotFound = 0xC000003A;
    constexpr uint32_t MutantNotOwned = 0xC0000046;
    constexpr uint32_t SemaphoreLimitExceeded = 0xC0000047;
    constexpr uint32_t NotSupported = 0xC00000BB;
    constexpr uint32_t FileIsADirectory = 0xC00000BA;
    constexpr uint32_t NotADirectory = 0xC0000103;
    constexpr uint32_t ProcedureNotFound = 0xC000007A;
    constexpr uint32_t DriverOrdinalNotFound = 0xC0000262;
}

// Win32-style error codes returned by XAM.
namespace xerror
{
    constexpr uint32_t Success = 0;
    constexpr uint32_t FileNotFound = 2;
    constexpr uint32_t PathNotFound = 3;
    constexpr uint32_t AccessDenied = 5;
    constexpr uint32_t InvalidHandle = 6;
    constexpr uint32_t NoMoreFiles = 0x12;
    constexpr uint32_t InvalidParameter = 0x57;
    constexpr uint32_t InsufficientBuffer = 0x7A;
    constexpr uint32_t AlreadyExists = 0xB7;
    constexpr uint32_t IoPending = 0x3E5;
    constexpr uint32_t DeviceNotConnected = 0x48F;
    constexpr uint32_t NotFound = 0x490;
    constexpr uint32_t Cancelled = 0x4C7;
    constexpr uint32_t NotLoggedOn = 0x4DD;
    constexpr uint32_t NoSuchUser = 0x525;
    constexpr uint32_t FunctionFailed = 0x65B;
    constexpr uint32_t Empty = 0x10D2;
}
