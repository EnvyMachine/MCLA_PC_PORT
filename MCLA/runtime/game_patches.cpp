// Replacements for individual recompiled game functions. Each recompiled function
// is a weak symbol sub_<address> aliasing __imp__sub_<address>; defining sub_<address>
// here overrides it while the original stays callable as __imp__sub_<address>.
#include "guest.h"
#include "log.h"

#include <ppc_recomp_shared.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace crash { void WatchWrites(uint32_t guestAddr, uint32_t maxHits, bool reads = false); }

extern "C" PPC_FUNC(__imp__sub_82360518);
extern "C" PPC_FUNC(__imp__sub_823317D8);
extern "C" PPC_FUNC(__imp__sub_822A9260);

// mc_FlushDataCache(address, size, invalidate): a dcbf/dcbst loop over the range.
// Caches are coherent on the host, so the loop is pure overhead.
PPC_FUNC(sub_821D5510)
{
}

// Menu list update (pause menu): dereferences the list's table source [list+0xC0]
// without a null check, which crashes on SAVE SETTINGS (found by LARecomp,
// MCLA_MenuListNullTableGuard at 0x8266126C). Return early like the function's own
// empty-list path does.
extern "C" PPC_FUNC(__imp__sub_82661210);
PPC_FUNC(sub_82661210)
{
    uint32_t self = ctx.r3.u32;
    // Same list selection as the function: UI form 0x48 (sub_82641CB0) decides between
    // the embedded list and the current page's.
    uint32_t ui = Load32(Load32(0x8286D804) + 0x30) + 0x13F80;
    uint32_t form = Load32(Load32(ui + 0x44) + 0x48 * 4);
    uint32_t list;
    if (form && Load8(form + 0xF6))
        list = self + 0x480;
    else
    {
        uint32_t pages = Load32(self + 0x164), page = Load32(self + 0x160);
        uint32_t entry = pages ? Load32(pages + page * 4) : 0;
        uint32_t holder = entry ? Load32(entry + 0x44) : 0;
        list = holder ? Load32(holder) : 0;
    }
    if (!list || !Load32(list + 0xC0))
    {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_82661210(ctx, base);
}

// ---- probes (MCLA_CAR_PROBE=1): why the player's car is not drawn -------------

namespace
{
    bool CarProbe()
    {
        static bool on = GetEnvironmentVariableA("MCLA_CAR_PROBE", nullptr, 0) > 0;
        return on;
    }

    struct CarDrawStats
    {
        uint32_t calls = 0, passes = 0, inst = 0, obj = 0, state = 0;
        uint32_t readyTrue = 0, readyFalse = 0, hiddenTrue = 0, hiddenFalse = 0;
    };
    std::mutex s_probeLock;
    std::map<uint32_t, CarDrawStats> s_carDraws;
    auto s_lastProbeLog = std::chrono::steady_clock::now();
    uint32_t s_currentCar;

    void MaybeLogProbes()
    {
        auto now = std::chrono::steady_clock::now();
        if (now - s_lastProbeLog < std::chrono::seconds(3))
            return;
        s_lastProbeLog = now;
        for (auto& [self, s] : s_carDraws)
        {
            LOGI("probe: mcCarModel %08X draw x%u passes %X inst %08X obj %08X state %u | ready %u/%u hidden %u/%u", self,
                 s.calls, s.passes, s.inst, s.obj, s.state, s.readyTrue, s.readyFalse, s.hiddenTrue, s.hiddenFalse);
            // Objects may have been freed since the draw; only touch plausible heap pointers.
            auto plausible = [](uint32_t p) { return p >= 0x40000000 && p < 0xC0000000; };
            if (plausible(s.inst) && plausible(Load32(s.inst + 8)))
            {
                LOGI("probe:   position %g %g %g / %g %g %g%s", LoadF32(s.inst + 0xC0), LoadF32(s.inst + 0xC4), LoadF32(s.inst + 0xC8),
                     LoadF32(s.inst + 0x100), LoadF32(s.inst + 0x104), LoadF32(s.inst + 0x108),
                     self == Load32(0x8288DCF8) ? " (player)" : "");
                // NaN scan of the objects around the car (vtable + offsets of NaN words).
                auto scan = [&](const char* what, uint32_t obj, uint32_t size) {
                    if (!plausible(obj) || !GetEnvironmentVariableA("MCLA_NAN_SCAN", nullptr, 0))
                        return;
                    std::string text;
                    char buf[16];
                    int n = 0;
                    for (uint32_t off = 0; off < size; off += 4)
                    {
                        uint32_t v = Load32(obj + off);
                        if ((v & 0x7F800000) == 0x7F800000 && (v & 0x7FFFFF) && n++ < 24)
                        {
                            snprintf(buf, sizeof(buf), " %X", off);
                            text += buf;
                        }
                    }
                    LOGI("probe:   %s %08X vtable %08X: %d NaN words%s", what, obj, Load32(obj), n, text.c_str());
                };
                uint32_t sim = Load32(s.inst + 8);
                // MCLA_CAR_DIFF: log the player car's words that changed since the last report.
                if (self == Load32(0x8288DCF8) && GetEnvironmentVariableA("MCLA_CAR_DIFF", nullptr, 0) > 0)
                {
                    static std::map<uint32_t, uint32_t> previous;
                    auto diff = [&](const char* what, uint32_t obj, uint32_t size) {
                        std::string text;
                        char buf[48];
                        int n = 0;
                        for (uint32_t off = 0; off < size; off += 4)
                        {
                            uint32_t v = Load32(obj + off), key = obj + off;
                            auto it = previous.find(key);
                            if (it != previous.end() && it->second != v && n++ < 48)
                            {
                                float f;
                                memcpy(&f, &v, 4);
                                snprintf(buf, sizeof(buf), " +%X=%08X(%g)", off, v, f);
                                text += buf;
                            }
                            previous[key] = v;
                        }
                        LOGI("probe:   diff %s %08X:%s", what, obj, text.c_str());
                    };
                    diff("car", s.inst, 0x1000);
                    diff("sim", sim, 0x800);
                }
                LOGI("probe:   sim +6C %08X +7C %08X +178 %08X +39C %08X +3AC %08X", Load32(sim + 0x6C), Load32(sim + 0x7C),
                     Load32(sim + 0x178), Load32(sim + 0x39C), Load32(sim + 0x3AC));
                static uint32_t watchedSim = 0;
                if (char v[16]; !watchedSim && GetEnvironmentVariableA("MCLA_WATCH_SIM", v, sizeof(v)) > 0)
                {
                    watchedSim = sim;
                    crash::WatchWrites(sim + strtoul(v, nullptr, 16), 16);
                }
                scan("inst", s.inst, 0x400);
                scan("obj", s.obj, 0x2000);
                scan("model", self, 0x2000);
                scan("inst+8", Load32(s.inst + 8), 0x400);
                scan("inst+0x44", Load32(s.inst + 0x44), 0x400);
                scan("inst+0x50", Load32(s.inst + 0x50), 0x400);
            }
        }
        s_carDraws.clear();
        // MCLA_WATCH_PAD: once RT is down, log who reads pad 0's trigger bytes.
        static bool padWatched = false;
        if (!padWatched && Load8(0x82905388 + 0x15) == 0xFF && GetEnvironmentVariableA("MCLA_WATCH_PAD", nullptr, 0) > 0)
        {
            padWatched = true;
            char v[16] = "14";
            GetEnvironmentVariableA("MCLA_WATCH_PAD", v, sizeof(v));
            crash::WatchWrites(0x82905388 + strtoul(v, nullptr, 16), 24, GetEnvironmentVariableA("MCLA_WATCH_WRITES", nullptr, 0) == 0);
        }
        for (uint32_t pad = 0; pad < 2; pad++)
        {
            uint32_t p = 0x82905388 + pad * 64;
            LOGI("probe: pad %u: buttons %08X sticks %02X %02X %02X %02X triggers %02X %02X connected %u type %u", pad,
                 Load32(p + 8), Load8(p + 0x10), Load8(p + 0x11), Load8(p + 0x12), Load8(p + 0x13), Load8(p + 0x14),
                 Load8(p + 0x15), Load8(p + 0x2C), Load32(p + 0x34));
        }
        void LogCallCounts();
        LogCallCounts();
        void LogVisibility();
        LogVisibility();
    }
}

// mcCarModel::Draw(pass)
PPC_FUNC(sub_82360518)
{
    if (!CarProbe())
        return __imp__sub_82360518(ctx, base);
    uint32_t self = ctx.r3.u32, pass = ctx.r4.u32;
    uint32_t inst = Load32(self + 0x14);
    uint32_t obj = inst ? Load32(inst + 0x80) : 0;
    {
        std::lock_guard lock(s_probeLock);
        CarDrawStats& s = s_carDraws[self];
        s.calls++;
        s.passes |= pass;
        s.inst = inst;
        s.obj = obj;
        s.state = obj ? Load32(obj + 0x290) : 0xFFFFFFFF;
        s_currentCar = self;
        MaybeLogProbes();
    }
    __imp__sub_82360518(ctx, base);
}

namespace
{
    std::map<uint32_t, uint32_t> s_calls;          // function -> calls
    std::map<uint32_t, uint32_t> s_partPasses;     // pass -> calls of sub_8235FB78
    uint32_t s_flagTrue, s_flagFalse, s_hidden1943, s_no74, s_no78;
    float s_fadeMin = 1e30f, s_fadeMax = -1e30f;

    void CountCall(uint32_t address)
    {
        std::lock_guard lock(s_probeLock);
        s_calls[address]++;
    }

    void LogCallCounts()
    {
        std::string text;
        char buf[48];
        for (auto& [a, n] : s_calls)
        {
            snprintf(buf, sizeof(buf), " %08X:%u", a, n);
            text += buf;
        }
        LOGI("probe: calls%s", text.c_str());
        text.clear();
        for (auto& [p, n] : s_partPasses)
        {
            snprintf(buf, sizeof(buf), " %X:%u", p, n);
            text += buf;
        }
        LOGI("probe: parts passes%s | flag %u/%u fade %g..%g | hidden1943 %u no74 %u no78 %u", text.c_str(), s_flagTrue,
             s_flagFalse, s_fadeMin, s_fadeMax, s_hidden1943, s_no74, s_no78);
        s_calls.clear();
        s_partPasses.clear();
        s_flagTrue = s_flagFalse = s_hidden1943 = s_no74 = s_no78 = 0;
        s_fadeMin = 1e30f;
        s_fadeMax = -1e30f;
    }
}

#define COUNT_PROBE(addr)                                                                          \
    extern "C" PPC_FUNC(__imp__sub_##addr);                                                        \
    PPC_FUNC(sub_##addr)                                                                           \
    {                                                                                              \
        if (CarProbe())                                                                            \
            CountCall(0x##addr);                                                                   \
        __imp__sub_##addr(ctx, base);                                                              \
    }

// mcPlayerInput virtuals
COUNT_PROBE(822A2300)
COUNT_PROBE(822A28C0)
COUNT_PROBE(8255FDB8)
COUNT_PROBE(822A0068)
COUNT_PROBE(8229FF08)
extern "C" PPC_FUNC(__imp__sub_822A2988);
PPC_FUNC(sub_822A2988)
{
    if (CarProbe())
    {
        CountCall(0x822A2988);
        static auto last = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(3))
        {
            last = std::chrono::steady_clock::now();
            uint32_t s = ctx.r3.u32;
            {
                static std::map<uint32_t, uint32_t> previous;
                std::string text;
                char buf[48];
                for (uint32_t off = 0; off < 0x120; off += 4)
                {
                    uint32_t v = Load32(s + off);
                    auto it = previous.find(off);
                    if (it == previous.end() || it->second != v)
                    {
                        float f;
                        memcpy(&f, &v, 4);
                        snprintf(buf, sizeof(buf), " +%X=%08X(%g)", off, v, f);
                        text += buf;
                    }
                    previous[off] = v;
                }
                LOGI("probe: player input changes:%s", text.c_str());
            }
            uint32_t a = Load32(0x828CCEA0), b = Load32(0x828CCFC4), c = Load32(0x82874374);
            uint32_t gate = c ? Load32(c + 0x238) : 0;
            uint32_t ui = Load32(0x8286D8D4) ? Load32(0x8286D8D4) + 0x13F80 : 0;
            for (uint32_t form = ui, depth = 0; form && depth < 8; depth++)
            {
                LOGI("probe: ui form %u: %08X vtable %08X fn2A4 %08X child list %08X", depth, form, Load32(form),
                     Load32(Load32(form) + 0x2A4), Load32(form + 0xD8));
                uint32_t node = Load32(form + 0xD8);
                form = node ? Load32(node) : 0;
            }
            LOGI("probe: player input %08X: +60 %08X +8 %04X pad %d +74 %08X +4C %d | CEA0 %08X [4]=%u [6]=%u [A]=%u | CFC4 %08X [4C]=%02X | gate %08X vtable %08X fn %08X",
                 s, Load32(s + 0x60), Load16(s + 8), int16_t(Load16(s + 0xA)), Load32(s + 0x74), int32_t(Load32(s + 0x4C)), a,
                 a ? Load8(a + 4) : 0, a ? Load8(a + 6) : 0, a ? Load8(a + 0xA) : 0, b, b ? Load8(b + 0x4C) : 0, gate,
                 gate ? Load32(gate) : 0, gate ? Load32(Load32(gate) + 4) : 0);
        }
    }
    __imp__sub_822A2988(ctx, base);
}
COUNT_PROBE(822A1738)
COUNT_PROBE(8255FFF8)
COUNT_PROBE(825602F0)
COUNT_PROBE(8235D500)
COUNT_PROBE(8235E0E0)
COUNT_PROBE(82359F58)
COUNT_PROBE(82359C10)
COUNT_PROBE(8235A628)
COUNT_PROBE(8235EEF8)
COUNT_PROBE(82358F50)
COUNT_PROBE(82352A38)
COUNT_PROBE(8235E3D8)
COUNT_PROBE(8235AB50)

// mcCarModel: draw the car's parts for one pass.
extern "C" PPC_FUNC(__imp__sub_8235FB78);
PPC_FUNC(sub_8235FB78)
{
    if (CarProbe())
    {
        uint32_t self = ctx.r3.u32;
        std::lock_guard lock(s_probeLock);
        s_partPasses[ctx.r4.u32]++;
        if (Load8(self + 0x1943))
            s_hidden1943++;
        if (!Load16(self + 0x74))
            s_no74++;
        if (!Load16(self + 0x78))
            s_no78++;
    }
    __imp__sub_8235FB78(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_822A91E0);
PPC_FUNC(sub_822A91E0)
{
    uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_822A91E0(ctx, base);
    if (CarProbe() && lr == 0x8235FE00)
    {
        std::lock_guard lock(s_probeLock);
        (ctx.r3.u8 ? s_flagTrue : s_flagFalse)++;
    }
}

extern "C" PPC_FUNC(__imp__sub_8237CAC8);
PPC_FUNC(sub_8237CAC8)
{
    uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_8237CAC8(ctx, base);
    if (CarProbe() && lr == 0x8235FE28)
    {
        std::lock_guard lock(s_probeLock);
        s_fadeMin = std::min(s_fadeMin, float(ctx.f1.f64));
        s_fadeMax = std::max(s_fadeMax, float(ctx.f1.f64));
    }
}

namespace
{
    uint32_t s_frustumIn, s_frustumOut, s_occlusionCalls;
    std::map<uint32_t, uint32_t> s_occlusionResults;
    float s_sphere[4];
    uint32_t s_viewport;

    void LogVisibility()
    {
        LOGI("probe: frustum in %u out %u, occlusion calls %u; last sphere %g %g %g r %g viewport %08X", s_frustumIn,
             s_frustumOut, s_occlusionCalls, s_sphere[0], s_sphere[1], s_sphere[2], s_sphere[3], s_viewport);
        s_frustumIn = s_frustumOut = 0;
    }
}

extern "C" PPC_FUNC(__imp__sub_8217C148);
PPC_FUNC(sub_8217C148)
{
    uint32_t lr = uint32_t(ctx.lr);
    if (CarProbe() && lr == 0x8235F924)
    {
        static int logged = 0;
        if (logged++ % 3000 == 0)
        {
            uint32_t m = ctx.r10.u32, box = ctx.r7.u32, sp = ctx.r1.u32;
            for (int row = 0; row < 4; row++)
                LOGI("probe: car matrix %08X row %d: %g %g %g %g", m, row, LoadF32(m + row * 16), LoadF32(m + row * 16 + 4),
                     LoadF32(m + row * 16 + 8), LoadF32(m + row * 16 + 12));
            for (int row = 0; row < 3; row++)
                LOGI("probe: car box %08X +%02X: %g %g %g %g", box, row * 16, LoadF32(box + row * 16), LoadF32(box + row * 16 + 4),
                     LoadF32(box + row * 16 + 8), LoadF32(box + row * 16 + 12));
            LOGI("probe: stack center %g %g %g | extent %g %g %g", LoadF32(sp + 0x50), LoadF32(sp + 0x54), LoadF32(sp + 0x58),
                 LoadF32(sp + 0x70), LoadF32(sp + 0x74), LoadF32(sp + 0x78));
        }
        static bool watching = false;
        float boxMin = LoadF32(ctx.r7.u32 + 0x10);
        if (!watching && boxMin != boxMin)
        {
            watching = true;
            crash::WatchWrites(ctx.r7.u32 + 0x10, 12);
        }
    }
    __imp__sub_8217C148(ctx, base);
    if (CarProbe() && lr == 0x8235F924)
    {
        std::lock_guard lock(s_probeLock);
        (ctx.r3.u32 ? s_frustumIn : s_frustumOut)++;
        for (int i = 0; i < 4; i++)
            s_sphere[i] = ctx.v1.f32[3 - i];
        s_viewport = ctx.r10.u32; // clobbered; viewport pointer logged from the caller below
        static int planesLogged = 0;
        if (planesLogged++ % 3000 == 0)
        {
            // r3 is overwritten with the result; the viewport is the caller's r29.
            uint32_t vp = ctx.r29.u32;
            for (int p = 0; p < 8; p++)
            {
                uint32_t a = vp + 0x360 + p * 16;
                LOGI("probe: viewport %08X plane %d: %g %g %g %g", vp, p, LoadF32(a), LoadF32(a + 4), LoadF32(a + 8),
                     LoadF32(a + 12));
            }
            LOGI("probe: sphere %g %g %g r %g -> %u", s_sphere[0], s_sphere[1], s_sphere[2], s_sphere[3], ctx.r3.u32);
        }
    }
}

extern "C" PPC_FUNC(__imp__sub_82175F20);
PPC_FUNC(sub_82175F20)
{
    uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_82175F20(ctx, base);
    if (CarProbe() && lr == 0x8235F9EC)
    {
        std::lock_guard lock(s_probeLock);
        s_occlusionResults[ctx.r3.u32]++;
        if (s_occlusionCalls++ % 2000 == 0)
        {
            std::string text;
            char buf[32];
            for (auto& [v, n] : s_occlusionResults)
            {
                snprintf(buf, sizeof(buf), " %08X:%u", v, n);
                text += buf;
            }
            LOGI("probe: car visibility: frustum in %u out %u, occlusion results%s", s_frustumIn, s_frustumOut, text.c_str());
        }
    }
}

// Input mapper: analog value of control r4 (2 = gas, 3 = brake in the player input update).
extern "C" PPC_FUNC(__imp__sub_822A4BB8);
PPC_FUNC(sub_822A4BB8)
{
    uint32_t control = ctx.r4.u32, lr = uint32_t(ctx.lr), mapper = ctx.r3.u32;
    if (CarProbe() && control == 2 && lr == 0x822A2CD8)
    {
        static int dumps = 0;
        if (dumps++ % 300 == 0)
        {
            uint32_t scheme = Load32(mapper + 4), table = Load32(0x8287438C + scheme * 4), values = Load32(mapper + 0xC);
            std::string text;
            char buf[48];
            for (uint32_t i = 0; i < 24 && table; i++)
            {
                snprintf(buf, sizeof(buf), " %u:%u/%u", i, Load32(table + i * 8), Load32(table + i * 8 + 4));
                text += buf;
            }
            LOGI("probe: mapper %08X scheme %u table %08X pad %08X values %08X:%s", mapper, scheme, table, Load32(mapper + 8),
                 values, text.c_str());
            if (values)
                LOGI("probe:   gas value bytes %02X %02X %02X %02X %08X %08X", Load8(values + 24), Load8(values + 25),
                     Load8(values + 26), Load8(values + 27), Load32(values + 28), Load32(values + 32));
        }
    }
    __imp__sub_822A4BB8(ctx, base);
    if (CarProbe() && (lr == 0x822A2CC8 || lr == 0x822A2CD8))
    {
        static float maxValue[4];
        static std::atomic<uint32_t> calls;
        if (control < 4)
            maxValue[control] = std::max(maxValue[control], float(ctx.f1.f64));
        if (calls++ % 90 == 0)
        {
            LOGI("probe: mapper gas max %g brake max %g", maxValue[2], maxValue[3]);
            maxValue[2] = maxValue[3] = 0;
            static int dumps = 0;
            if (dumps++ < 2)
            {
                uint32_t mapper = ctx.r3.u32; // clobbered? log from the saved value below
                (void)mapper;
            }
        }
    }
}

// Vehicle input from a pad ([this+0xA] = pad index).
extern "C" PPC_FUNC(__imp__sub_82560640);
PPC_FUNC(sub_82560640)
{
    if (CarProbe())
    {
        static std::atomic<uint32_t> calls;
        static auto last = std::chrono::steady_clock::now();
        calls++;
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(3))
        {
            last = std::chrono::steady_clock::now();
            uint32_t self = ctx.r3.u32;
            LOGI("probe: vehicle input %08X x%u pad %d flags38 %u %u lr %08X", self, calls.exchange(0), int16_t(Load16(self + 0xA)),
                 Load8(self + 0x38), Load8(self + 0x39), uint32_t(ctx.lr));
        }
    }
    __imp__sub_82560640(ctx, base);
}

// Draw-time "instance is ready" predicate (false skips the draw).
PPC_FUNC(sub_823317D8)
{
    uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_823317D8(ctx, base);
    if (CarProbe() && lr == 0x82360554)
    {
        std::lock_guard lock(s_probeLock);
        (ctx.r3.u8 ? s_carDraws[s_currentCar].readyTrue : s_carDraws[s_currentCar].readyFalse)++;
    }
}

// Draw-time predicate on the instance's object (true skips the draw).
PPC_FUNC(sub_822A9260)
{
    uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_822A9260(ctx, base);
    if (CarProbe() && lr == 0x82360578)
    {
        std::lock_guard lock(s_probeLock);
        (ctx.r3.u8 ? s_carDraws[s_currentCar].hiddenTrue : s_carDraws[s_currentCar].hiddenFalse)++;
    }
}

// Shader parameter lookup by name (effect, name, ...) -> handle, 0 if absent. The
// post-processing setup (0x8260D0B8, 0x8260D0D4) looks up the full-screen speed
// blur's "blurEffectTintFade" and "blurEffectDesatScale"; without the handles the
// effect stays off (as the community "Disable Motion Blur" patch does). Off unless
// MCLA_MOTION_BLUR=1.
extern "C" PPC_FUNC(__imp__sub_8218A568);
PPC_FUNC(sub_8218A568)
{
    static const bool motionBlur = [] {
        char v[8];
        return GetEnvironmentVariableA("MCLA_MOTION_BLUR", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    uint32_t name = ctx.r4.u32;
    if (!motionBlur && (name == 0x82088CD8 || name == 0x82088CC0))
    {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_8218A568(ctx, base);
}
