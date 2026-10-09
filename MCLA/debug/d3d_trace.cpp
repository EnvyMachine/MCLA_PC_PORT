// Entry tracing for identifying D3D functions by behaviour (tools/gen_d3d_trace.py).
// MCLA_D3D_TRACE=<seconds>: after that long, writes notes/d3d_trace.txt with, per
// traced function: calls, calls per presented frame, how often r3 was the D3D
// device, the most common callers, and the first argument sets seen.
#include "../gpu/command_processor.h"
#include "../runtime/guest.h"
#include "../runtime/loader.h"
#include "../runtime/log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

extern const uint32_t kD3DTracedFunctions[];
extern const uint32_t kD3DTracedFunctionCount;

namespace crash { extern thread_local bool t_probing; }

namespace
{
    constexpr int kCallers = 6;
    constexpr int kSamples = 4;

    struct Stats
    {
        std::atomic<uint64_t> calls{ 0 };
        std::atomic<uint64_t> deviceCalls{ 0 };
        std::atomic<uint64_t> containerArgs[4] = {}; // r3..r6 pointed at a shader container
        std::mutex lock;
        uint32_t callers[kCallers] = {};
        uint64_t callerCounts[kCallers] = {};
        uint32_t samples[kSamples][7] = {}; // r3..r8, lr
        int sampleCount = 0;
    };

    std::vector<Stats>* s_stats;
    bool s_enabled = false;

    // Whether a guest pointer addresses a D3D shader container; argument registers
    // hold arbitrary values, so the read is guarded.
    bool IsShaderContainer(uint32_t addr)
    {
        if (addr < 0x00010000 || (addr & 3))
            return false;
        crash::t_probing = true;
        bool result = false;
        __try
        {
            result = (Load32(addr) & 0xFFFFFF00) == 0x102A1100;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        crash::t_probing = false;
        return result;
    }

    void Dump(const std::filesystem::path& path, uint64_t frames)
    {
        FILE* f = _wfopen(path.c_str(), L"w");
        if (!f)
            return;
        uint32_t device = Load32(g_xex.vdGlobalDevice);
        fprintf(f, "# D3D entry trace: %llu frames presented, device %08X\n", frames, device);
        // The device's setter tables: render states at device + 0x40 (0x65 entries,
        // indexed by D3DRS value / 4), sampler states at device + 0x1D4 (0x14 entries,
        // indexed by D3DSAMP value; called as fn(device, sampler, value)).
        if (device)
        {
            fprintf(f, "# render-state setters (index: function)\n");
            for (uint32_t i = 0; i < 0x65; i++)
                fprintf(f, "#   RS %3u (0x%03X): %08X\n", i, i * 4, Load32(device + 0x40 + i * 4));
            fprintf(f, "# sampler-state setters (index: function)\n");
            for (uint32_t i = 0; i < 0x14; i++)
                fprintf(f, "#   SS %3u: %08X\n", i, Load32(device + 0x1D4 + i * 4));
        }
        fprintf(f, "# functions given a D3D shader container (flags 0x102A11xx) as an argument\n");
        for (uint32_t i = 0; i < kD3DTracedFunctionCount; i++)
        {
            Stats& s = (*s_stats)[i];
            for (int r = 0; r < 4; r++)
                if (s.containerArgs[r])
                    fprintf(f, "#   %08X r%d x%llu\n", kD3DTracedFunctions[i], r + 3, s.containerArgs[r].load());
        }
        fprintf(f, "# func      calls  /frame  dev%%  callers(count) | samples r3 r4 r5 r6 r7 r8 lr\n");
        std::vector<uint32_t> order;
        for (uint32_t i = 0; i < kD3DTracedFunctionCount; i++)
            if ((*s_stats)[i].calls)
                order.push_back(i);
        std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return kD3DTracedFunctions[a] < kD3DTracedFunctions[b]; });
        for (uint32_t i : order)
        {
            Stats& s = (*s_stats)[i];
            std::lock_guard lock(s.lock);
            uint64_t calls = s.calls;
            fprintf(f, "%08X %9llu %7.1f %4.0f ", kD3DTracedFunctions[i], calls, frames ? double(calls) / frames : 0.0,
                    100.0 * s.deviceCalls / calls);
            for (int c = 0; c < kCallers && s.callers[c]; c++)
                fprintf(f, " %08X(%llu)", s.callers[c], s.callerCounts[c]);
            fprintf(f, "\n");
            for (int k = 0; k < s.sampleCount; k++)
                fprintf(f, "        | %08X %08X %08X %08X %08X %08X lr=%08X\n", s.samples[k][0], s.samples[k][1],
                        s.samples[k][2], s.samples[k][3], s.samples[k][4], s.samples[k][5], s.samples[k][6]);
        }
        fclose(f);
        LOGI("d3d trace: %zu functions called, written to %s", order.size(), path.string().c_str());
    }

    struct Init
    {
        Init()
        {
            char v[16];
            if (!kD3DTracedFunctionCount || GetEnvironmentVariableA("MCLA_D3D_TRACE", v, sizeof(v)) == 0)
                return;
            int seconds = atoi(v);
            s_stats = new std::vector<Stats>(kD3DTracedFunctionCount);
            s_enabled = true;
            std::thread([seconds] {
                auto start = std::chrono::steady_clock::now();
                uint64_t frames0 = gpu::FramesPresented();
                std::this_thread::sleep_until(start + std::chrono::seconds(seconds));
                wchar_t exe[MAX_PATH];
                GetModuleFileNameW(nullptr, exe, MAX_PATH);
                auto root = std::filesystem::path(exe).parent_path().parent_path().parent_path().parent_path();
                Dump(root / "notes" / "d3d_trace.txt", gpu::FramesPresented() - frames0);
            }).detach();
        }
    } s_init;
}

void D3DTraceRecord(uint32_t index, PPCContext& ctx)
{
    if (!s_enabled)
        return;
    Stats& s = (*s_stats)[index];
    s.calls.fetch_add(1, std::memory_order_relaxed);
    if (ctx.r3.u32 && ctx.r3.u32 == Load32(g_xex.vdGlobalDevice))
        s.deviceCalls.fetch_add(1, std::memory_order_relaxed);
    const uint32_t args[4] = { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32 };
    for (int r = 0; r < 4; r++)
        if (IsShaderContainer(args[r]))
            s.containerArgs[r].fetch_add(1, std::memory_order_relaxed);
    uint32_t caller = uint32_t(ctx.lr);
    std::lock_guard lock(s.lock);
    int slot = -1;
    for (int c = 0; c < kCallers; c++)
    {
        if (s.callers[c] == caller || !s.callers[c])
        {
            slot = c;
            break;
        }
    }
    if (slot >= 0)
    {
        s.callers[slot] = caller;
        s.callerCounts[slot]++;
    }
    if (s.sampleCount < kSamples)
    {
        uint32_t* d = s.samples[s.sampleCount++];
        d[0] = ctx.r3.u32; d[1] = ctx.r4.u32; d[2] = ctx.r5.u32; d[3] = ctx.r6.u32;
        d[4] = ctx.r7.u32; d[5] = ctx.r8.u32; d[6] = caller;
    }
}
