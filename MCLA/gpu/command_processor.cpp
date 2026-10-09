#include "command_processor.h"
#include "shader_registry.h"
#include "../render/device.h"
#include "../render/renderer.h"
#include "../runtime/guest.h"
#include "../runtime/log.h"
#include "../runtime/memory.h"
#include "../runtime/thread.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace gpu
{
    namespace
    {
        constexpr uint32_t kRegisterBase = 0x7FC80000;
        constexpr uint32_t kRegisterCount = 0x5003;

        // Register indices
        constexpr uint32_t CP_RB_WPTR = 0x01C5;
        constexpr uint32_t SCRATCH_UMSK = 0x01DC;
        constexpr uint32_t SCRATCH_ADDR = 0x01DD;
        constexpr uint32_t WRITEBACK_START = 0x0A04;
        constexpr uint32_t WRITEBACK_SIZE = 0x0A05;
        constexpr uint32_t COHER_STATUS_HOST = 0x0A31;
        constexpr uint32_t SCRATCH_REG0 = 0x0578;
        constexpr uint32_t VGT_EVENT_INITIATOR = 0x21F9;
        constexpr uint32_t RB_SAMPLE_COUNT_ADDR = 0x2325;
        constexpr uint32_t SHADER_CONSTANT_FETCH_00_0 = 0x4800;

        // Type-3 opcodes
        enum : uint32_t
        {
            PM4_ME_INIT = 0x48, PM4_NOP = 0x10, PM4_INTERRUPT = 0x54, PM4_XE_SWAP = 0x64,
            PM4_INDIRECT_BUFFER = 0x3F, PM4_INDIRECT_BUFFER_PFD = 0x37,
            PM4_WAIT_REG_MEM = 0x3C, PM4_REG_RMW = 0x21, PM4_REG_TO_MEM = 0x3E,
            PM4_MEM_WRITE = 0x3D, PM4_COND_WRITE = 0x45, PM4_EVENT_WRITE = 0x46,
            PM4_EVENT_WRITE_SHD = 0x58, PM4_EVENT_WRITE_EXT = 0x5A, PM4_EVENT_WRITE_ZPD = 0x5B,
            PM4_DRAW_INDX = 0x22, PM4_DRAW_INDX_2 = 0x36,
            PM4_SET_BIN_MASK = 0x50, PM4_SET_BIN_SELECT = 0x51,
            PM4_SET_BIN_MASK_LO = 0x60, PM4_SET_BIN_MASK_HI = 0x61,
            PM4_SET_BIN_SELECT_LO = 0x62, PM4_SET_BIN_SELECT_HI = 0x63,
            PM4_SET_CONSTANT = 0x2D, PM4_SET_CONSTANT2 = 0x55, PM4_SET_SHADER_CONSTANTS = 0x56,
            PM4_LOAD_ALU_CONSTANT = 0x2F, PM4_IM_LOAD = 0x27, PM4_IM_LOAD_IMMEDIATE = 0x2B,
        };

        // SET_CONSTANT / LOAD_ALU_CONSTANT target: register block per constant type.
        constexpr uint32_t kConstantTypeBase[5] = {
            0x4000, // ALU (float4 constants, VS 0-255 then PS 256-511)
            0x4800, // fetch constants (32 x 6 dwords)
            0x4900, // bool constants
            0x4908, // loop constants
            0x2000, // ordinary registers
        };

        // Shaders the next draw uses (PM4 IM_LOAD / IM_LOAD_IMMEDIATE).
        struct ActiveShader
        {
            uint32_t code = 0;            // guest address of the microcode as loaded
            uint32_t dwords = 0;
            bool immediate = false;       // copied inline into the command stream
            GuestShader* shader = nullptr; // registry entry (container), if known
        };
        ActiveShader s_activeShader[2]; // [0] vertex, [1] pixel

        // Predication (tiled rendering replays one stream per tile): a type-3 packet
        // with bit 0 set only executes if (bin_select & bin_mask) != 0.
        uint64_t s_binMask = 0xFFFFFFFFull, s_binSelect = 0xFFFFFFFFull;

        constexpr uint32_t kSwapSignature = 0x53574150; // 'SWAP'

        std::atomic<uint32_t> s_ringPhys, s_ringSizeDwords, s_rptrWriteback, s_ringGeneration;
        std::atomic<uint32_t> s_interruptCallback, s_interruptContext;
        std::atomic<uint64_t> s_frames, s_draws, s_eventCounter;
        std::once_flag s_startOnce;
        PPCContext* s_ctx;

        uint32_t PhysToVirt(uint32_t phys) { return 0xA0000000 | (phys & 0x1FFFFFFF); }

        uint32_t ReadReg(uint32_t index) { return Load32(kRegisterBase + index * 4); }
        void StoreReg(uint32_t index, uint32_t value) { Store32(kRegisterBase + index * 4, value); }

        // Xenos endian swap modes (low 2 bits of an address operand).
        uint32_t GpuSwap(uint32_t value, uint32_t endian)
        {
            switch (endian & 3)
            {
            case 1: return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF); // 8in16
            case 2: return __builtin_bswap32(value);                                  // 8in32
            case 3: return (value >> 16) | (value << 16);                              // 16in32
            default: return value;
            }
        }

        // GPU memory accesses are little-endian with an optional swap mode.
        uint32_t GpuRead(uint32_t physAddress)
        {
            uint32_t raw;
            memcpy(&raw, g_base + PhysToVirt(physAddress & ~3u), 4);
            return GpuSwap(raw, physAddress & 3);
        }

        void GpuWrite(uint32_t physAddress, uint32_t value)
        {
            static int logged = 0;
            if (logged++ < 60)
                LOGI("gpu: write %08X -> phys %08X", value, physAddress);
            uint32_t data = GpuSwap(value, physAddress & 3);
            memcpy(g_base + PhysToVirt(physAddress & ~3u), &data, 4);
        }

        // Runs the D3D interrupt handler on the calling runtime thread (the command
        // processor for CP interrupts, the vblank thread for vsync).
        void Interrupt(PPCContext& ctx, uint32_t source, uint32_t cpu)
        {
            uint32_t cb = s_interruptCallback.load();
            if (!cb)
                return;
            // An interrupt can't run while a thread on its CPU has IRQL raised, and
            // interrupts aimed at one CPU never overlap (vblank and CP both use CPU 2).
            std::lock_guard lock(thread::CpuInterruptLock(cpu));
            // The handler acknowledges by clearing bit (1 << current cpu), read from
            // PCR->prcb.current_cpu, so run it "on" the CPU the interrupt targets,
            // at raised IRQL.
            GuestThread* t = thread::Current();
            if (t)
            {
                Store8(t->pcr + 0x10C, uint8_t(cpu));
                Store8(t->kthread + 0xBF, uint8_t(cpu));
                Store8(t->pcr + 0x18, 2);
            }
            ctx.r3.u64 = source;
            ctx.r4.u64 = s_interruptContext.load();
            if (source == 1)
            {
                static int logged = 0;
                uint32_t block = Load32(s_interruptContext.load() + 0x2A94);
                if (logged++ < 12)
                    LOGI("gpu: CP interrupt cpu %u: block[0]=%08X callback=%08X arg=%08X", cpu, Load32(block),
                         Load32(block + 0x10), Load32(block + 0x14));
            }
            CallGuest(ctx, cb);
            if (t)
                Store8(t->pcr + 0x18, 0);
            if (source == 1)
            {
                // MCLA's D3D: per-CPU deferred-callback ring (4 entries) at
                // device + 0x2C3C + cpu * 0x50; write count +0x38, read count +0x3C.
                uint32_t q = s_interruptContext.load() + 0x2C3C + cpu * 0x50;
                uint32_t pending = Load32(q + 0x38) - Load32(q + 0x3C);
                static int logged = 0;
                if (pending > 4 && logged++ < 20)
                    LOGI("gpu: deferred callback ring for cpu %u overflowed (%u pending)", cpu, pending);
            }
        }

        void Interrupt(uint32_t source, uint32_t cpu)
        {
            if (s_ctx)
                Interrupt(*s_ctx, source, cpu);
        }

        // Vblank runs independently of the command stream (the GPU may be blocked
        // waiting for a flip that only the vblank handler completes).
        void VblankWorker()
        {
            PPCContext& ctx = thread::AttachHostThread("MCLA vblank");
            SetThreadDescription(GetCurrentThread(), L"MCLA vblank");
            auto next = std::chrono::steady_clock::now();
            for (;;)
            {
                next += std::chrono::microseconds(16667);
                std::this_thread::sleep_until(next);
                if (std::chrono::steady_clock::now() > next + std::chrono::milliseconds(100))
                    next = std::chrono::steady_clock::now(); // don't burst after a stall
                Interrupt(ctx, 0, 2);
            }
        }

        void WriteRegister(uint32_t index, uint32_t value)
        {
            if (index >= kRegisterCount)
                return;
            if (index == COHER_STATUS_HOST)
                value |= 0x80000000;
            StoreReg(index, value);
            if (index >= SCRATCH_REG0 && index < SCRATCH_REG0 + 8)
            {
                uint32_t n = index - SCRATCH_REG0;
                static int logged = 0;
                if (logged++ < 20)
                    LOGI("gpu: SCRATCH_REG%u = %08X (umsk %08X addr %08X)", n, value, ReadReg(SCRATCH_UMSK), ReadReg(SCRATCH_ADDR));
                if (ReadReg(SCRATCH_UMSK) & (1u << n))
                    Store32(PhysToVirt(ReadReg(SCRATCH_ADDR) + n * 4), value);
            }
        }

        bool Match(uint32_t value, uint32_t ref, uint32_t waitInfo)
        {
            switch (waitInfo & 7)
            {
            case 0: return false;
            case 1: return value < ref;
            case 2: return value <= ref;
            case 3: return value == ref;
            case 4: return value != ref;
            case 5: return value >= ref;
            case 6: return value > ref;
            default: return true;
            }
        }

        // Reads dwords from either the (circular) primary buffer or a linear buffer.
        struct Reader
        {
            uint32_t base;      // guest virtual address
            uint32_t sizeDwords; // 0 = linear
            uint32_t pos;
            uint32_t remaining;

            uint32_t Read()
            {
                uint32_t index = sizeDwords ? pos % sizeDwords : pos;
                pos++;
                remaining--;
                return Load32(base + index * 4);
            }

            void Skip(uint32_t n)
            {
                pos += n;
                remaining -= n;
            }
        };

        void ExecuteBuffer(Reader& r, int depth);

        // Current chain of buffers being executed (for parse-error reports).
        struct BufferFrame { uint32_t base, length; const Reader* reader; };
        BufferFrame s_chain[8];
        int s_chainDepth = 0;

        // Development census of the first draws: which shaders they load (and
        // whether the registry knows them) and the render-target state.
        void DrawCensus(uint32_t initiator, uint32_t indexBase, uint32_t indexSize)
        {
            static int logged = 0;
            static uint64_t draws = 0, hits[2], misses[2];
            draws++;
            for (int k = 0; k < 2; k++)
            {
                const ActiveShader& a = s_activeShader[k];
                bool known = a.shader != nullptr;
                (known ? hits : misses)[k]++;
                static uint32_t missLogged[2][16];
                static int missCount[2];
                // Unknown shaders are told apart by their first instruction words.
                uint32_t key = a.code ? Load32(a.code) ^ Load32(a.code + 4) ^ a.dwords : 0;
                if (!known && missCount[k] < 16)
                {
                    bool seen = false;
                    for (int i = 0; i < missCount[k]; i++)
                        seen |= missLogged[k][i] == key;
                    if (!seen)
                    {
                        missLogged[k][missCount[k]++] = key;
                        LOGI("gpu: draw %llu uses an unregistered %s%s shader at %08X, %u dwords (IB %08X)", draws,
                             a.immediate ? "inline " : "", k ? "pixel" : "vertex", a.code, a.dwords,
                             s_chainDepth > 0 ? s_chain[s_chainDepth - 1].base : 0);
                    }
                }
            }
            if (logged < 4)
            {
                logged++;
                LOGI("gpu: draw %llu: initiator %08X (prim %u, src %u, %u indices) dma %08X/%08X | surface %08X color %08X "
                     "depth %08X mode %08X copy %08X->%08X | vs %08X ps %08X",
                     draws, initiator, initiator & 0x3F, (initiator >> 6) & 3, initiator >> 16, indexBase, indexSize,
                     ReadReg(0x2000), ReadReg(0x2001), ReadReg(0x2002), ReadReg(0x2208), ReadReg(0x2318), ReadReg(0x2319),
                     s_activeShader[0].code, s_activeShader[1].code);
            }
            if ((draws & 0x3FFFFF) == 0) // every ~4M draws (a couple of minutes of play)
                LOGI("gpu: %llu draws; vertex shaders known %llu / unknown %llu, pixel %llu / %llu; %zu shaders registered",
                     draws, hits[0], misses[0], hits[1], misses[1], ShaderCount());
        }

        void LogChain(const char* what)
        {
            static int logged = 0;
            if (logged++ >= 6)
                return;
            LOGI("gpu: %s; buffer chain:", what);
            for (int i = 0; i < s_chainDepth; i++)
            {
                uint32_t pos = s_chain[i].reader ? s_chain[i].reader->pos : 0;
                uint32_t size = s_chain[i].reader ? s_chain[i].reader->sizeDwords : 0;
                auto at = [&](int d) {
                    uint32_t idx = pos + d;
                    if (size)
                        idx %= size;
                    return Load32(s_chain[i].base + idx * 4);
                };
                LOGI("gpu:   [%d] base %08X len %u, at dword %u; preceding: %08X %08X %08X %08X %08X %08X", i,
                     s_chain[i].base, s_chain[i].length, pos, at(-6), at(-5), at(-4), at(-3), at(-2), at(-1));
            }
            if (logged == 1 && s_chainDepth > 1)
            {
                // Re-walk the failing buffer (sizes only) and show the last packet headers
                // before the failure, to find the packet whose length was misjudged.
                const BufferFrame& f = s_chain[s_chainDepth - 1];
                uint32_t at = f.reader->pos;
                struct Hdr { uint32_t pos, packet, size; };
                Hdr history[32];
                uint32_t n = 0;
                for (uint32_t pos = 0; pos < at && pos < f.length;)
                {
                    uint32_t p = Load32(f.base + pos * 4);
                    uint32_t size = 1;
                    switch (p >> 30)
                    {
                    case 0: size = (p == 0 || p == 0x0BADF00D) ? 1 : ((p >> 16) & 0x3FFF) + 2; break;
                    case 1: size = 3; break;
                    case 2: size = 1; break;
                    case 3: size = ((p >> 16) & 0x3FFF) + 2; break;
                    }
                    if (p >> 30 != 2 && p != 0)
                        history[n++ % 32] = { pos, p, size };
                    pos += size;
                }
                for (uint32_t i = (n > 32 ? n - 32 : 0); i < n; i++)
                {
                    const Hdr& h = history[i % 32];
                    LOGI("gpu:   @%6u %08X type %u op %02X size %u  next: %08X %08X", h.pos, h.packet, h.packet >> 30,
                         (h.packet >> 8) & 0x7F, h.size, Load32(f.base + (h.pos + 1) * 4), Load32(f.base + (h.pos + 2) * 4));
                }
            }
        }

        void ExecuteIndirect(uint32_t phys, uint32_t lengthDwords, int depth)
        {
            if (depth > 4)
            {
                LOGI("gpu: indirect buffer nesting too deep");
                return;
            }
            Reader r{ PhysToVirt(phys), 0, 0, lengthDwords };
            if (s_chainDepth < 8)
                s_chain[s_chainDepth] = { r.base, lengthDwords, &r };
            s_chainDepth++;
            ExecuteBuffer(r, depth + 1);
            s_chainDepth--;
        }

        bool ExecuteType3(Reader& r, uint32_t packet, int depth)
        {
            uint32_t opcode = (packet >> 8) & 0x7F;
            uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
            static bool seen[128];
            if (!seen[opcode])
            {
                seen[opcode] = true;
                LOGI("gpu: first type-3 opcode %02X (count %u)", opcode, count);
            }
            if (r.remaining < count)
            {
                char what[96];
                snprintf(what, sizeof(what), "type-3 packet %08X needs %u dwords, %u left", packet, count, r.remaining);
                LogChain(what);
                return false;
            }
            // Predicated packets run only for selected bins; predicated swaps never (as in Xenia).
            bool skipped = (packet & 1) && ((s_binSelect & s_binMask) == 0 || opcode == PM4_XE_SWAP);
            if (render::LoggingFrame() && opcode != PM4_DRAW_INDX && opcode != PM4_DRAW_INDX_2)
            {
                uint32_t d[3] = {};
                for (uint32_t i = 0; i < 3 && i < count; i++)
                    d[i] = Load32(r.base + (r.sizeDwords ? (r.pos + i) % r.sizeDwords : r.pos + i) * 4);
                LOGI("pm4: %08X+%u op %02X x%u%s%s: %08X %08X %08X", r.base, r.pos - 1, opcode, count,
                     (packet & 1) ? " pred" : "", skipped ? " SKIPPED" : "", d[0], d[1], d[2]);
            }
            if (skipped)
            {
                if ((opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_2) && render::LoggingFrame())
                    LOGI("pm4: %08X+%u draw skipped by predication (bin mask %016llX select %016llX)", r.base, r.pos - 1,
                         (unsigned long long)s_binMask, (unsigned long long)s_binSelect);
                r.Skip(count);
                return true;
            }

            switch (opcode)
            {
            case PM4_SET_BIN_MASK_LO:
                s_binMask = (s_binMask & 0xFFFFFFFF00000000ull) | r.Read();
                r.Skip(count - 1);
                return true;
            case PM4_SET_BIN_MASK_HI:
                s_binMask = (s_binMask & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32);
                r.Skip(count - 1);
                return true;
            case PM4_SET_BIN_SELECT_LO:
                s_binSelect = (s_binSelect & 0xFFFFFFFF00000000ull) | r.Read();
                r.Skip(count - 1);
                return true;
            case PM4_SET_BIN_SELECT_HI:
                s_binSelect = (s_binSelect & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32);
                r.Skip(count - 1);
                return true;
            case PM4_SET_BIN_MASK:
            case PM4_SET_BIN_SELECT:
            {
                uint64_t hi = r.Read(), lo = r.Read();
                r.Skip(count - 2);
                (opcode == PM4_SET_BIN_MASK ? s_binMask : s_binSelect) = (hi << 32) | lo;
                return true;
            }
            case PM4_INTERRUPT:
            {
                uint32_t cpuMask = r.Read();
                r.Skip(count - 1);
                static int logged = 0;
                if (logged++ < 10)
                    LOGI("gpu: INTERRUPT packet, cpu mask %X", cpuMask);
                for (uint32_t n = 0; n < 6; n++)
                    if (cpuMask & (1u << n))
                        Interrupt(1, n);
                return true;
            }
            case PM4_XE_SWAP:
            {
                // Written by WriteSwapPacket: signature, front buffer (physical), width, height.
                uint32_t signature = r.Read(), frontBuffer = r.Read(), width = r.Read(), height = r.Read();
                r.Skip(count - 4);
                s_frames++;
                s_eventCounter++;
                if (render::DeviceReady() && signature == kSwapSignature)
                    render::Swap(frontBuffer, width, height);
                return true;
            }
            case PM4_INDIRECT_BUFFER:
            case PM4_INDIRECT_BUFFER_PFD:
            {
                uint32_t addr = r.Read();
                uint32_t rawLength = r.Read();
                uint32_t length = rawLength & 0xFFFFF;
                r.Skip(count - 2);
                static int logged = 0;
                if (logged++ < 12)
                    LOGI("gpu: IB%s %08X len %08X (from %08X+%u, depth %d)", opcode == PM4_INDIRECT_BUFFER_PFD ? "_PFD" : "",
                         addr, rawLength, r.base, r.pos - 3, depth);
                ExecuteIndirect(mem::ToPhysical(addr), length, depth);
                return true;
            }
            case PM4_WAIT_REG_MEM:
            {
                uint32_t waitInfo = r.Read(), pollAddr = r.Read(), ref = r.Read(), mask = r.Read(), wait = r.Read();
                r.Skip(count - 5);
                // Blocks until satisfied, as on hardware: CPU threads use this to hold the
                // GPU until they finish writing command buffers it is about to execute.
                auto lastLog = std::chrono::steady_clock::now();
                for (;;)
                {
                    uint32_t value;
                    if (waitInfo & 0x10)
                        value = GpuRead(pollAddr);
                    else
                    {
                        if (pollAddr == COHER_STATUS_HOST)
                            StoreReg(COHER_STATUS_HOST, 0); // caches are always coherent here
                        value = ReadReg(pollAddr);
                    }
                    if (Match(value & mask, ref, waitInfo))
                        break;
                    if (std::chrono::steady_clock::now() - lastLog > std::chrono::seconds(5))
                    {
                        lastLog = std::chrono::steady_clock::now();
                        LOGI("gpu: still waiting (WAIT_REG_MEM on %s %08X, info %08X ref %08X mask %08X, value %08X)",
                             (waitInfo & 0x10) ? "mem" : "reg", pollAddr, waitInfo, ref, mask, value);
                    }
                    // The wait interval is in GPU clocks; the CPU side usually releases the
                    // wait within microseconds, so poll instead of sleeping whole milliseconds.
                    if (std::chrono::steady_clock::now() - lastLog > std::chrono::milliseconds(2) && wait >= 0x100)
                        std::this_thread::sleep_for(std::chrono::microseconds(500));
                    else
                        std::this_thread::yield();
                }
                return true;
            }
            case PM4_REG_RMW:
            {
                uint32_t info = r.Read(), andMask = r.Read(), orMask = r.Read();
                r.Skip(count - 3);
                uint32_t value = ReadReg(info & 0x1FFF);
                value &= (info >> 31) ? ReadReg(andMask & 0x1FFF) : andMask;
                value |= ((info >> 30) & 1) ? ReadReg(orMask & 0x1FFF) : orMask;
                WriteRegister(info & 0x1FFF, value);
                return true;
            }
            case PM4_REG_TO_MEM:
            {
                uint32_t reg = r.Read(), addr = r.Read();
                r.Skip(count - 2);
                if (reg < kRegisterCount)
                    GpuWrite(addr, ReadReg(reg));
                return true;
            }
            case PM4_MEM_WRITE:
            {
                uint32_t addr = r.Read();
                for (uint32_t i = 1; i < count; i++, addr += 4)
                    GpuWrite(addr, r.Read());
                return true;
            }
            case PM4_COND_WRITE:
            {
                uint32_t waitInfo = r.Read(), pollAddr = r.Read(), ref = r.Read(), mask = r.Read();
                uint32_t writeAddr = r.Read(), data = r.Read();
                r.Skip(count - 6);
                uint32_t value = (waitInfo & 0x10) ? GpuRead(pollAddr) : ReadReg(pollAddr);
                if (Match(value & mask, ref, waitInfo))
                {
                    if (waitInfo & 0x100)
                        GpuWrite(writeAddr, data);
                    else
                        WriteRegister(writeAddr, data);
                }
                return true;
            }
            case PM4_EVENT_WRITE:
                StoreReg(VGT_EVENT_INITIATOR, r.Read() & 0x3F);
                r.Skip(count - 1);
                return true;
            case PM4_EVENT_WRITE_SHD:
            {
                uint32_t initiator = r.Read(), addr = r.Read(), value = r.Read();
                r.Skip(count - 3);
                StoreReg(VGT_EVENT_INITIATOR, initiator & 0x3F);
                uint32_t data = (initiator >> 31) ? uint32_t(s_eventCounter.load()) : value;
                uint32_t target = addr & ~3u;
                uint32_t wbStart = ReadReg(WRITEBACK_START), wbSize = ReadReg(WRITEBACK_SIZE);
                if (target > 0x1FFFFFFF && wbStart && target - wbStart < wbSize)
                {
                    uint32_t swapped = GpuSwap(data, addr & 3);
                    memcpy(g_base + 0x7F000000 + (target - wbStart), &swapped, 4);
                }
                else
                    GpuWrite(addr, data);
                return true;
            }
            case PM4_EVENT_WRITE_EXT:
            {
                uint32_t initiator = r.Read(), addr = r.Read();
                r.Skip(count - 2);
                StoreReg(VGT_EVENT_INITIATOR, initiator & 0x3F);
                // Screen extents of previous draws: report "everything" (8in16 halves).
                const uint16_t extents[6] = { 0, 8192 >> 3, 0, 8192 >> 3, 0, 1 };
                uint32_t dst = PhysToVirt(addr & ~3u);
                for (int i = 0; i < 6; i++)
                    Store16(dst + i * 2, extents[i]);
                return true;
            }
            case PM4_EVENT_WRITE_ZPD:
            {
                // Occlusion queries: the GPU writes its running sample counters to
                // RB_SAMPLE_COUNT_ADDR and D3D subtracts a query's BEGIN report from its
                // END report. With no real counting, every report advances the "passed"
                // counter so every query sees visible samples - games cull objects
                // (MCLA: the player's car, traffic, pedestrians) whose queries read 0.
                uint32_t initiator = r.Read();
                r.Skip(count - 1);
                StoreReg(VGT_EVENT_INITIATOR, initiator & 0x3F);
                uint32_t reportAddress = ReadReg(RB_SAMPLE_COUNT_ADDR);
                if (!reportAddress)
                    return true;
                static uint32_t s_zpdPassed = 0;
                s_zpdPassed += 1000;
                // The report is little-endian (D3D swaps it); A + B lanes are summed.
                // Total and ZFail first, ZPass (what readers wait on) last.
                uint32_t a = s_zpdPassed - (s_zpdPassed >> 1), b = s_zpdPassed >> 1;
                uint32_t head[4] = { a, b, 0, 0 }, tail[4] = { a, b, 0, 0 };
                uint8_t* report = g_base + PhysToVirt(reportAddress & ~3u);
                memcpy(report, head, sizeof(head));
                std::atomic_thread_fence(std::memory_order_release);
                memcpy(report + 16, tail, sizeof(tail));
                return true;
            }
            case PM4_SET_CONSTANT:
            {
                uint32_t offsetType = r.Read();
                uint32_t type = (offsetType >> 16) & 0xFF;
                uint32_t index = offsetType & 0x7FF;
                if (type > 4)
                {
                    r.Skip(count - 1);
                    return true;
                }
                for (uint32_t i = 1; i < count; i++)
                    WriteRegister(kConstantTypeBase[type] + index++, r.Read());
                return true;
            }
            case PM4_SET_CONSTANT2:
            case PM4_SET_SHADER_CONSTANTS:
            {
                uint32_t index = r.Read() & 0xFFFF;
                for (uint32_t i = 1; i < count; i++)
                    WriteRegister(index++, r.Read());
                return true;
            }
            case PM4_LOAD_ALU_CONSTANT:
            {
                uint32_t address = r.Read() & 0x3FFFFFFF, offsetType = r.Read(), sizeDwords = r.Read() & 0xFFF;
                r.Skip(count - 3);
                uint32_t type = (offsetType >> 16) & 0xFF;
                uint32_t index = offsetType & 0x7FF;
                if (type <= 4)
                    for (uint32_t i = 0; i < sizeDwords; i++)
                        WriteRegister(kConstantTypeBase[type] + index + i, Load32(PhysToVirt(address) + i * 4));
                return true;
            }
            case PM4_IM_LOAD:
            {
                uint32_t addrType = r.Read(), startSize = r.Read();
                r.Skip(count - 2);
                uint32_t type = addrType & 3; // 0 vertex, 1 pixel
                uint32_t phys = mem::ToPhysical(addrType & ~3u);
                uint32_t dwords = startSize & 0xFFFF;
                if (type < 2)
                {
                    GuestShader* shader = FindShaderByMicrocode(phys, type == 1);
                    if (!shader)
                        shader = FindShaderByProgram(PhysToVirt(phys), dwords, type == 1);
                    if (!shader && dwords)
                        shader = GetRawShader(PhysToVirt(phys), dwords, type == 1);
                    s_activeShader[type] = { PhysToVirt(phys), dwords, false, shader };
                }
                return true;
            }
            case PM4_IM_LOAD_IMMEDIATE:
            {
                uint32_t type = r.Read(), startSize = r.Read();
                uint32_t code = r.base + (r.sizeDwords ? r.pos % r.sizeDwords : r.pos) * 4;
                uint32_t dwords = startSize & 0xFFFF;
                r.Skip(count - 2);
                if (type < 2)
                {
                    GuestShader* shader = FindShaderByInlineCopy(mem::ToPhysical(code), type == 1);
                    if (!shader && dwords <= count - 2)
                        shader = FindShaderByProgram(code, dwords, type == 1);
                    if (!shader && dwords && dwords <= count - 2)
                        shader = GetRawShader(code, dwords, type == 1);
                    s_activeShader[type] = { code, dwords, true, shader };
                }
                return true;
            }
            case PM4_DRAW_INDX:
            case PM4_DRAW_INDX_2:
            {
                s_draws++;
                uint32_t used = 0;
                if (opcode == PM4_DRAW_INDX)
                {
                    r.Read(); // viz query token
                    used++;
                }
                uint32_t initiator = r.Read();
                used++;
                StoreReg(0x21FC, initiator); // VGT_DRAW_INITIATOR
                uint32_t indexBase = 0, indexSize = 0;
                if (((initiator >> 6) & 3) == 0 && count - used >= 2) // indices from memory (DMA)
                {
                    indexBase = r.Read();
                    indexSize = r.Read();
                    used += 2;
                }
                r.Skip(count - used);
                DrawCensus(initiator, indexBase, indexSize);
                if (render::DeviceReady())
                {
                    render::DrawPacket packet;
                    packet.initiator = initiator;
                    packet.indexBase = indexBase;
                    packet.indexSize = indexSize;
                    packet.vs = s_activeShader[0].shader;
                    packet.vsCode = s_activeShader[0].code;
                    packet.vsDwords = s_activeShader[0].dwords;
                    packet.ps = s_activeShader[1].shader;
                    packet.psCode = s_activeShader[1].code;
                    packet.psDwords = s_activeShader[1].dwords;
                    render::Draw(packet);
                }
                return true;
            }
            default:
                // State/constant/shader loads and other packets without CPU-visible
                // effects are skipped until there is a renderer.
                r.Skip(count);
                return true;
            }
        }

        void ExecuteBuffer(Reader& r, int depth)
        {
            while (r.remaining > 0)
            {
                uint32_t packet = r.Read();
                if (packet == 0 || packet == 0x0BADF00D)
                    continue;
                switch (packet >> 30)
                {
                case 0:
                {
                    uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
                    if (r.remaining < count)
                    {
                        char what[96];
                        snprintf(what, sizeof(what), "type-0 packet %08X needs %u dwords, %u left", packet, count, r.remaining);
                        LogChain(what);
                        return;
                    }
                    uint32_t base = packet & 0x7FFF;
                    bool oneReg = (packet >> 15) & 1;
                    for (uint32_t i = 0; i < count; i++)
                        WriteRegister(oneReg ? base : base + i, r.Read());
                    break;
                }
                case 1:
                {
                    uint32_t a = r.Read(), b = r.Read();
                    WriteRegister(packet & 0x7FF, a);
                    WriteRegister((packet >> 11) & 0x7FF, b);
                    break;
                }
                case 2:
                    break;
                case 3:
                    if (!ExecuteType3(r, packet, depth))
                        return;
                    break;
                }
            }
        }

        void Worker()
        {
            s_ctx = &thread::AttachHostThread("MCLA gpu");
            SetThreadDescription(GetCurrentThread(), L"MCLA gpu");
            uint32_t rptr = 0, generation = s_ringGeneration.load();
            for (;;)
            {
                if (s_ringGeneration.load() != generation) // ring re-initialised: start over
                {
                    generation = s_ringGeneration.load();
                    rptr = 0;
                }
                uint32_t size = s_ringSizeDwords.load();
                uint32_t wptr = ReadReg(CP_RB_WPTR);
                if (size && wptr != rptr && wptr < size)
                {
                    Reader r{ PhysToVirt(s_ringPhys.load()), size, rptr, (wptr + size - rptr) % size };
                    static int dumps = 0;
                    if (dumps++ < 2)
                    {
                        std::string s;
                        char buf[16];
                        for (uint32_t i = rptr; i != wptr && s.size() < 900; i = (i + 1) % size)
                        {
                            snprintf(buf, sizeof(buf), "%08X ", Load32(PhysToVirt(s_ringPhys.load()) + i * 4));
                            s += buf;
                        }
                        LOGI("gpu: ring %u..%u: %s", rptr, wptr, s.c_str());
                    }
                    s_chain[0] = { r.base, size, &r };
                    s_chainDepth = 1;
                    ExecuteBuffer(r, 0);
                    s_chainDepth = 0;
                    rptr = wptr;
                    if (uint32_t wb = s_rptrWriteback.load())
                        Store32(PhysToVirt(wb), rptr);
                }

                // The guest often blocks on the GPU right after a kick (fences, ring space),
                // so poll closely for a while before falling back to short sleeps.
                auto idleStart = std::chrono::steady_clock::now();
                while (ReadReg(CP_RB_WPTR) == rptr && s_ringGeneration.load() == generation)
                {
                    if (std::chrono::steady_clock::now() - idleStart < std::chrono::milliseconds(2))
                    {
                        for (int i = 0; i < 64; i++)
                            _mm_pause();
                        std::this_thread::yield();
                    }
                    else
                        std::this_thread::sleep_for(std::chrono::microseconds(500));
                }
            }
        }

        void Start()
        {
            std::call_once(s_startOnce, [] {
                // Registers the D3D library reads back (values from Xenia's GraphicsSystem).
                StoreReg(0x0F00, 0x08100748); // RB_EDRAM_TIMING
                StoreReg(0x0F01, 0x0000200E); // RB_BC_CONTROL
                StoreReg(0x0F2D, 0x00BBBBBB); // RB_HSIO_INTERFACE_ALIGNER_VALUE
                StoreReg(0x1951, 1);          // interrupt status: vblank
                StoreReg(0x1961, 0x050002D0); // AVIVO_D1MODE_VIEWPORT_SIZE (1280x720)
                std::thread(Worker).detach();
                std::thread(VblankWorker).detach();
            });
        }
    }

    void InitializeRingBuffer(uint32_t physAddress, uint32_t sizeLog2)
    {
        uint32_t sizeBytes = 1u << (sizeLog2 + 3);
        memset(g_base + PhysToVirt(physAddress), 0, sizeBytes);
        s_ringPhys = mem::ToPhysical(physAddress);
        s_ringSizeDwords = sizeBytes / 4;
        s_ringGeneration++;
        LOGI("gpu: ring buffer at physical %08X, %u bytes", s_ringPhys.load(), sizeBytes);
        Start();
    }

    void EnableReadPointerWriteBack(uint32_t physAddress, uint32_t blockSizeLog2)
    {
        s_rptrWriteback = mem::ToPhysical(physAddress);
        LOGI("gpu: read pointer writeback at physical %08X", s_rptrWriteback.load());
        Start();
    }

    void SetInterruptCallback(uint32_t callback, uint32_t context)
    {
        s_interruptContext = context;
        s_interruptCallback = callback;
        LOGI("gpu: interrupt callback %08X (context %08X)", callback, context);
        Start();
    }

    void WriteSwapPacket(uint32_t buffer, uint32_t fetchConstant, uint32_t frontBufferAddress, uint32_t width, uint32_t height)
    {
        uint32_t i = 0;
        auto put = [&](uint32_t v) { Store32(buffer + 4 * i++, v); };
        // Type-0: the six dwords of the front buffer's texture fetch constant.
        put((5u << 16) | SHADER_CONSTANT_FETCH_00_0);
        for (int k = 0; k < 6; k++)
            put(Load32(fetchConstant + k * 4));
        // Type-3 XE_SWAP: signature, front buffer (physical), width, height.
        put((3u << 30) | (3u << 16) | (PM4_XE_SWAP << 8));
        put(kSwapSignature);
        put(mem::ToPhysical(frontBufferAddress));
        put(width);
        put(height);
        while (i < 64)
            put(2u << 30); // type-2 NOPs
    }

    uint64_t FramesPresented() { return s_frames.load(); }
    uint64_t DrawCount() { return s_draws.load(); }
    uint32_t ReadRegister(uint32_t index) { return ReadReg(index); }
}
