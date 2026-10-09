// Runtime library, string/time helpers, debug output, module/XEX queries and
// miscellaneous kernel imports.
#include "imports.h"
#include "../runtime/vfs.h"

#include <windows.h>
#include <bcrypt.h>

#include <cwctype>

namespace
{
    // Variadic arguments for the kernel's printf family. Integers and doubles both
    // travel in 64-bit slots: GPRs r3-r10 first, then the caller's stack.
    struct GuestArgs
    {
        PPCContext* ctx = nullptr;
        uint32_t vaList = 0; // non-zero: va_list (array of be64 slots)
        int index = 0;

        uint64_t Next()
        {
            if (vaList)
                return Load64(vaList + 8 * index++);
            int i = index++;
            if (i < 8)
            {
                PPCRegister* regs[] = { &ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7, &ctx->r8, &ctx->r9, &ctx->r10 };
                return regs[i]->u64;
            }
            return Load64(ctx->r1.u32 + 0x50 + (i - 8) * 8);
        }
    };

    // Formats a guest printf-style string. Handles the conversions the CRT uses
    // (d i u x X o c s S p f e g n %, flags, width/precision incl. '*', h/l/ll/I64).
    std::string GuestFormat(uint32_t fmtAddr, GuestArgs args)
    {
        std::string out;
        const char* f = GuestPtr<const char>(fmtAddr);
        if (!f)
            return out;
        while (*f)
        {
            if (*f != '%')
            {
                out += *f++;
                continue;
            }
            const char* specStart = f++;
            if (*f == '%')
            {
                out += '%';
                f++;
                continue;
            }
            std::string spec = "%";
            while (*f && strchr("-+ #0", *f))
                spec += *f++;
            if (*f == '*')
            {
                spec += std::to_string(int32_t(args.Next()));
                f++;
            }
            else
                while (isdigit(uint8_t(*f)))
                    spec += *f++;
            if (*f == '.')
            {
                spec += *f++;
                if (*f == '*')
                {
                    spec += std::to_string(int32_t(args.Next()));
                    f++;
                }
                else
                    while (isdigit(uint8_t(*f)))
                        spec += *f++;
            }
            bool isLong64 = false, isShort = false, isWide = false;
            for (;;)
            {
                if (f[0] == 'l' && f[1] == 'l') { isLong64 = true; f += 2; }
                else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') { isLong64 = true; f += 3; }
                else if (*f == 'h') { isShort = true; f++; }
                else if (*f == 'l' || *f == 'w') { isWide = true; f++; }
                else if (*f == 'L' || *f == 'z' || *f == 't' || *f == 'j') f++;
                else break;
            }
            char conv = *f ? *f++ : 0;
            char buf[512];
            switch (conv)
            {
            case 'd': case 'i':
            {
                uint64_t v = args.Next();
                int64_t s = isLong64 ? int64_t(v) : isShort ? int16_t(v) : int32_t(v);
                snprintf(buf, sizeof(buf), (spec + "lld").c_str(), (long long)s);
                out += buf;
                break;
            }
            case 'u': case 'x': case 'X': case 'o':
            {
                uint64_t v = args.Next();
                uint64_t u = isLong64 ? v : isShort ? uint16_t(v) : uint32_t(v);
                snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), (unsigned long long)u);
                out += buf;
                break;
            }
            case 'p':
                snprintf(buf, sizeof(buf), "%08X", uint32_t(args.Next()));
                out += buf;
                break;
            case 'c':
                out += char(args.Next());
                break;
            case 's': case 'S':
            {
                uint32_t p = uint32_t(args.Next());
                bool wide = (conv == 'S') != isWide;
                std::string s;
                if (!p)
                    s = "(null)";
                else if (wide)
                    for (uint32_t a = p; Load16(a); a += 2)
                        s += char(Load16(a) & 0xFF);
                else
                    s = GuestPtr<const char>(p);
                snprintf(buf, sizeof(buf), (spec + "s").c_str(), s.c_str());
                out += buf;
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            {
                uint64_t bits = args.Next();
                double d;
                memcpy(&d, &bits, 8);
                snprintf(buf, sizeof(buf), (spec + conv).c_str(), d);
                out += buf;
                break;
            }
            case 'n':
                if (uint32_t p = uint32_t(args.Next()))
                    Store32(p, uint32_t(out.size()));
                break;
            default:
                out.append(specStart, f);
                break;
            }
        }
        return out;
    }

    uint32_t WriteFormatted(const std::string& s, uint32_t buffer, uint32_t count, bool bounded)
    {
        if (!bounded)
        {
            memcpy(g_base + buffer, s.c_str(), s.size() + 1);
            return uint32_t(s.size());
        }
        if (s.size() < count)
        {
            memcpy(g_base + buffer, s.c_str(), s.size() + 1);
            return uint32_t(s.size());
        }
        if (count)
            memcpy(g_base + buffer, s.data(), count); // MSVC _snprintf: no terminator when truncated
        return 0xFFFFFFFF;
    }
}

// ---- formatting ----------------------------------------------------------------

GUEST_IMPORT(sprintf)
{
    TRACE("sprintf");
    std::string s = GuestFormat(ctx.r4.u32, GuestArgs{ &ctx, 0, 2 });
    ctx.r3.u64 = WriteFormatted(s, ctx.r3.u32, 0, false);
}

GUEST_IMPORT(_snprintf)
{
    TRACE("_snprintf");
    std::string s = GuestFormat(ctx.r5.u32, GuestArgs{ &ctx, 0, 3 });
    ctx.r3.u64 = WriteFormatted(s, ctx.r3.u32, ctx.r4.u32, true);
}

GUEST_IMPORT(_vsnprintf)
{
    TRACE("_vsnprintf");
    std::string s = GuestFormat(ctx.r5.u32, GuestArgs{ &ctx, ctx.r6.u32, 0 });
    ctx.r3.u64 = WriteFormatted(s, ctx.r3.u32, ctx.r4.u32, true);
}

GUEST_IMPORT(DbgPrint)
{
    std::string s = GuestFormat(ctx.r3.u32, GuestArgs{ &ctx, 0, 1 });
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    LOGI("DbgPrint: %s", s.c_str());
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(DbgBreakPoint)
{
    LOGI("DbgBreakPoint (ignored), lr=%08X", uint32_t(ctx.lr));
}

// ---- strings -------------------------------------------------------------------

GUEST_IMPORT(RtlInitAnsiString)
{
    TRACE("RtlInitAnsiString");
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint16_t len = src ? uint16_t(strlen(GuestPtr<const char>(src))) : 0;
    Store16(dst + 0, len);
    Store16(dst + 2, src ? uint16_t(len + 1) : 0);
    Store32(dst + 4, src);
}

GUEST_IMPORT(RtlCompareStringN)
{
    TRACE("RtlCompareStringN");
    // (string1, length1, string2, length2, case_insensitive)
    const char* a = GuestPtr<const char>(ctx.r3.u32);
    const char* b = GuestPtr<const char>(ctx.r5.u32);
    uint32_t la = ctx.r4.u32 == 0xFFFFFFFF ? uint32_t(strlen(a)) : ctx.r4.u32;
    uint32_t lb = ctx.r6.u32 == 0xFFFFFFFF ? uint32_t(strlen(b)) : ctx.r6.u32;
    bool ci = ctx.r7.u32 != 0;
    int result = 0;
    for (uint32_t i = 0; i < std::min(la, lb) && !result; i++)
    {
        int ca = uint8_t(a[i]), cb = uint8_t(b[i]);
        if (ci)
            ca = toupper(ca), cb = toupper(cb);
        result = ca - cb;
    }
    if (!result)
        result = int(la) - int(lb);
    ctx.r3.s64 = result;
}

GUEST_IMPORT(RtlCompareMemoryUlong)
{
    TRACE("RtlCompareMemoryUlong");
    uint32_t src = ctx.r3.u32, length = ctx.r4.u32, pattern = ctx.r5.u32;
    uint32_t matched = 0;
    while (matched + 4 <= length && Load32(src + matched) == pattern)
        matched += 4;
    ctx.r3.u64 = matched;
}

GUEST_IMPORT(RtlFillMemoryUlong)
{
    TRACE("RtlFillMemoryUlong");
    for (uint32_t i = 0; i + 4 <= ctx.r4.u32; i += 4)
        Store32(ctx.r3.u32 + i, ctx.r5.u32);
}

GUEST_IMPORT(RtlMultiByteToUnicodeN)
{
    TRACE("RtlMultiByteToUnicodeN");
    // (unicode_dst, max_bytes, bytes_written*, multibyte_src, src_bytes)
    uint32_t count = std::min(ctx.r4.u32 / 2, ctx.r7.u32);
    for (uint32_t i = 0; i < count; i++)
        Store16(ctx.r3.u32 + i * 2, Load8(ctx.r6.u32 + i));
    if (ctx.r5.u32)
        Store32(ctx.r5.u32, count * 2);
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(RtlUnicodeToMultiByteN)
{
    TRACE("RtlUnicodeToMultiByteN");
    // (multibyte_dst, max_bytes, bytes_written*, unicode_src, src_bytes)
    uint32_t count = std::min(ctx.r4.u32, ctx.r7.u32 / 2);
    for (uint32_t i = 0; i < count; i++)
    {
        uint16_t c = Load16(ctx.r6.u32 + i * 2);
        Store8(ctx.r3.u32 + i, c < 0x100 ? uint8_t(c) : '?');
    }
    if (ctx.r5.u32)
        Store32(ctx.r5.u32, count);
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(RtlUpcaseUnicodeChar)
{
    TRACE("RtlUpcaseUnicodeChar");
    ctx.r3.u64 = uint16_t(towupper(wchar_t(ctx.r3.u32 & 0xFFFF)));
}

GUEST_IMPORT(RtlNtStatusToDosError)
{
    TRACE("RtlNtStatusToDosError");
    uint32_t s = ctx.r3.u32, e;
    switch (s)
    {
    case xstatus::Success: e = 0; break;
    case xstatus::Pending: e = 997; break;
    case xstatus::Timeout: e = 258; break;
    case xstatus::BufferOverflow: e = 234; break;
    case xstatus::NoMoreFiles: e = 18; break;
    case xstatus::NotImplemented: e = 120; break;
    case xstatus::InvalidHandle: e = 6; break;
    case xstatus::InvalidParameter: e = 87; break;
    case xstatus::NoSuchFile:
    case xstatus::ObjectNameNotFound: e = 2; break;
    case xstatus::ObjectPathNotFound: e = 3; break;
    case xstatus::EndOfFile: e = 38; break;
    case xstatus::NoMemory: e = 8; break;
    case xstatus::AccessDenied: e = 5; break;
    case xstatus::ObjectNameCollision: e = 183; break;
    case xstatus::BufferTooSmall: e = 122; break;
    default: e = (s & 0xFFFF0000) == 0x80070000 ? (s & 0xFFFF) : 317; break;
    }
    ctx.r3.u64 = e;
}

// ---- time ----------------------------------------------------------------------
// TIME_FIELDS: be16 year, month, day, hour, minute, second, milliseconds, weekday

GUEST_IMPORT(RtlTimeToTimeFields)
{
    TRACE("RtlTimeToTimeFields");
    uint64_t t = Load64(ctx.r3.u32);
    FILETIME ft{ DWORD(t), DWORD(t >> 32) };
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    uint32_t out = ctx.r4.u32;
    Store16(out + 0, st.wYear);
    Store16(out + 2, st.wMonth);
    Store16(out + 4, st.wDay);
    Store16(out + 6, st.wHour);
    Store16(out + 8, st.wMinute);
    Store16(out + 10, st.wSecond);
    Store16(out + 12, st.wMilliseconds);
    Store16(out + 14, st.wDayOfWeek);
}

GUEST_IMPORT(RtlTimeFieldsToTime)
{
    TRACE("RtlTimeFieldsToTime");
    uint32_t in = ctx.r3.u32;
    SYSTEMTIME st{};
    st.wYear = Load16(in + 0);
    st.wMonth = Load16(in + 2);
    st.wDay = Load16(in + 4);
    st.wHour = Load16(in + 6);
    st.wMinute = Load16(in + 8);
    st.wSecond = Load16(in + 10);
    st.wMilliseconds = Load16(in + 12);
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft))
    {
        ctx.r3.u64 = 0;
        return;
    }
    Store64(ctx.r4.u32, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    ctx.r3.u64 = 1;
}

// ---- exceptions ----------------------------------------------------------------

GUEST_IMPORT(RtlRaiseException)
{
    TRACE("RtlRaiseException");
    // EXCEPTION_RECORD: be32 code, flags, record, address, num_params, info[...]
    uint32_t rec = ctx.r3.u32;
    uint32_t code = Load32(rec);
    if (code == 0x406D1388) // MSVC SetThreadName
    {
        uint32_t namePtr = Load32(rec + 0x14 + 4);
        if (GuestThread* t = thread::Current(); t && namePtr)
        {
            t->name = GuestPtr<const char>(namePtr);
            std::wstring w(t->name.begin(), t->name.end());
            SetThreadDescription(GetCurrentThread(), w.c_str());
            LOGI("thread %u named '%s'", t->id, t->name.c_str());
        }
        return;
    }
    LOGI("RtlRaiseException: code %08X at %08X (continuing)", code, Load32(rec + 12));
}

GUEST_IMPORT(RtlUnwind)
{
    LOGI("RtlUnwind called (unsupported) lr=%08X", uint32_t(ctx.lr));
}

GUEST_IMPORT(__C_specific_handler)
{
    LOGI("__C_specific_handler called (unsupported) lr=%08X", uint32_t(ctx.lr));
    ctx.r3.u64 = 1; // ExceptionContinueSearch
}

// ---- XEX / module queries ------------------------------------------------------

GUEST_IMPORT(RtlImageXexHeaderField)
{
    TRACE("RtlImageXexHeaderField");
    uint32_t header = ctx.r3.u32;
    if (!header || Load32(header) != 0x58455832) // 'XEX2'
        header = g_xex.header;
    ctx.r3.u64 = XexHeaderField(header, ctx.r4.u32);
}

GUEST_IMPORT(XexGetModuleHandle)
{
    TRACE("XexGetModuleHandle");
    std::string name = ctx.r3.u32 ? GuestPtr<const char>(ctx.r3.u32) : "";
    if (name.empty() || _stricmp(name.c_str(), "default.xex") == 0)
    {
        Store32(ctx.r4.u32, g_xex.ldrEntry);
        ctx.r3.u64 = xstatus::Success;
        return;
    }
    LOGI("XexGetModuleHandle('%s'): not available", name.c_str());
    Store32(ctx.r4.u32, 0);
    ctx.r3.u64 = xstatus::ObjectNameNotFound;
}

GUEST_IMPORT(XexGetProcedureAddress)
{
    TRACE("XexGetProcedureAddress");
    LOGI("XexGetProcedureAddress(module %08X, ordinal %X): not available", ctx.r3.u32, ctx.r4.u32);
    if (ctx.r5.u32)
        Store32(ctx.r5.u32, 0);
    ctx.r3.u64 = xstatus::DriverOrdinalNotFound;
}

GUEST_IMPORT(XexCheckExecutablePrivilege)
{
    TRACE("XexCheckExecutablePrivilege");
    uint32_t privilege = ctx.r3.u32;
    ctx.r3.u64 = privilege < 32 ? (g_xex.systemFlags >> privilege) & 1 : 0;
}

GUEST_IMPORT(XexLoadImage)
{
    LOGI("XexLoadImage('%s'): not supported", ctx.r3.u32 ? GuestPtr<const char>(ctx.r3.u32) : "");
    ctx.r3.u64 = xstatus::NoSuchFile;
}

GUEST_IMPORT(XexLoadImageHeaders)
{
    LOGI("XexLoadImageHeaders: not supported");
    ctx.r3.u64 = xstatus::NoSuchFile;
}

GUEST_IMPORT(XexUnloadImage)
{
    TRACE("XexUnloadImage");
    ctx.r3.u64 = xstatus::Success;
}

// ---- configuration and system ---------------------------------------------------

GUEST_IMPORT(ExGetXConfigSetting)
{
    TRACE("ExGetXConfigSetting");
    uint16_t category = uint16_t(ctx.r3.u32), setting = uint16_t(ctx.r4.u32);
    uint32_t buffer = ctx.r5.u32, size = ctx.r6.u32 & 0xFFFF, pRequired = ctx.r7.u32;
    uint32_t value = 0, valueSize = 4;
    bool known = true;
    switch ((category << 16) | setting)
    {
    case 0x00020002: value = 0x00001000; break; // SECURED: AV region (NTSC-M)
    case 0x00020003: value = 0x000000FF; break; // SECURED: game region (North America)
    case 0x00030009: value = 1; break;          // USER: language (English)
    case 0x0003000A: value = 0x00040000; break; // USER: video flags (widescreen)
    case 0x0003000B: value = 0; break;          // USER: audio flags (stereo)
    case 0x0003000C: value = 0x40; break;       // USER: retail flags
    case 0x0003000E: value = 103; break;        // USER: country (United States)
    case 0x0003000F: value = 0; break;          // USER: parental control flags
    default: known = false; break;
    }
    if (!known)
        LOGI("ExGetXConfigSetting: unknown category %u setting %u (returning 0)", category, setting);
    if (pRequired)
        Store16(pRequired, uint16_t(valueSize));
    if (buffer)
    {
        if (size < valueSize)
        {
            ctx.r3.u64 = xstatus::BufferTooSmall;
            return;
        }
        Store32(buffer, value);
    }
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(XGetAVPack) { TRACE("XGetAVPack"); ctx.r3.u64 = 6; }
GUEST_IMPORT(XGetGameRegion) { TRACE("XGetGameRegion"); ctx.r3.u64 = 0xFF; }
GUEST_IMPORT(XGetLanguage) { TRACE("XGetLanguage"); ctx.r3.u64 = 1; }

GUEST_IMPORT(XGetVideoMode)
{
    TRACE("XGetVideoMode");
    uint32_t m = ctx.r3.u32;
    memset(g_base + m, 0, 0x30);
    Store32(m + 0x00, 1280);
    Store32(m + 0x04, 720);
    Store32(m + 0x08, 0);     // interlaced
    Store32(m + 0x0C, 1);     // widescreen
    Store32(m + 0x10, 1);     // high definition
    StoreF32(m + 0x14, 60.0f);
    Store32(m + 0x18, 1);     // NTSC
    Store32(m + 0x1C, 0x4A);
    Store32(m + 0x20, 0x01);
}

GUEST_IMPORT(HalReturnToFirmware)
{
    LOGI("HalReturnToFirmware(%u): title exited", ctx.r3.u32);
    LogFlush();
    ExitProcess(0);
}

GUEST_IMPORT(ExRegisterTitleTerminateNotification)
{
    TRACE("ExRegisterTitleTerminateNotification");
}

// ---- crypto --------------------------------------------------------------------

GUEST_IMPORT(XeCryptSha)
{
    TRACE("XeCryptSha");
    // (input1, size1, input2, size2, input3, size3, output, output_size)
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    uint8_t digest[20] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
    {
        uint32_t parts[3][2] = { { ctx.r3.u32, ctx.r4.u32 }, { ctx.r5.u32, ctx.r6.u32 }, { ctx.r7.u32, ctx.r8.u32 } };
        for (auto& p : parts)
            if (p[0] && p[1])
                BCryptHashData(hash, g_base + p[0], p[1], 0);
        BCryptFinishHash(hash, digest, sizeof(digest), 0);
        BCryptDestroyHash(hash);
    }
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    memcpy(g_base + ctx.r9.u32, digest, std::min<uint32_t>(ctx.r10.u32, 20));
}

GUEST_IMPORT(XeKeysConsolePrivateKeySign)
{
    TRACE("XeKeysConsolePrivateKeySign");
    // Console signatures can't be produced off-console; save data is never checked here.
    if (ctx.r4.u32)
        memset(g_base + ctx.r4.u32, 0, 0x228);
    ctx.r3.u64 = 1;
}

GUEST_IMPORT(XeKeysConsoleSignatureVerification)
{
    TRACE("XeKeysConsoleSignatureVerification");
    if (ctx.r5.u32)
        Store32(ctx.r5.u32, 1);
    ctx.r3.u64 = xstatus::Success;
}
