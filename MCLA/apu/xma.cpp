// XMA hardware decoder emulation.
//
// The title talks to the decoder through little-endian MMIO registers at
// 0x7FEA0000 (index*4). The recompiled code has no MMIO, so the few instructions
// that touch the side-effecting registers are routed here with XenonRecomp
// mid-asm hooks (config/MCLA.toml):
//   kick  (0x650+n): start decoding the contexts whose bits are set
//   lock  (0x690+n): stop decoding them
//   clear (0x6A0+n): reset them
//   CurrentContextIndex (0x606) read: rotating value, so lock code never sees a
//   context stuck "in progress".
// Context data (XMA_CONTEXT_DATA, 64 bytes each) lives in a physical array whose
// address is published in register 0x600.
//
// Decoding uses FFmpeg's XMA2 decoder (apu/ffmpeg.cpp) one packet at a time. Without
// FFmpeg it falls back to Xenia's "fake" decoder: input packets are consumed and
// silence is written to the output ring, enough for the audio engine to progress.
#include "audio_out.h"
#include "ffmpeg.h"
#include "../kernel/imports.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    constexpr uint32_t kRegisterBase = 0x7FEA0000;
    constexpr uint32_t kRegContextArrayAddress = 0x600;
    constexpr uint32_t kRegCurrentContextIndex = 0x606;
    constexpr uint32_t kRegKick = 0x650;
    constexpr uint32_t kRegLock = 0x690;
    constexpr uint32_t kRegClear = 0x6A0;
    constexpr uint32_t kContextCount = 320;

    constexpr uint32_t kBytesPerPacket = 2048;
    constexpr uint32_t kBitsPerPacket = kBytesPerPacket * 8;
    constexpr uint32_t kBitsPerPacketHeader = 32;
    constexpr uint32_t kOutputBytesPerBlock = 256;

    // Layout from Xenia's xma_context.h (bitfields allocated LSB first).
    struct ContextData
    {
        // DWORD 0
        uint32_t input_buffer_0_packet_count : 12;
        uint32_t loop_count : 8;
        uint32_t input_buffer_0_valid : 1;
        uint32_t input_buffer_1_valid : 1;
        uint32_t output_buffer_block_count : 5;
        uint32_t output_buffer_write_offset : 5;
        // DWORD 1
        uint32_t input_buffer_1_packet_count : 12;
        uint32_t loop_subframe_start : 2;
        uint32_t loop_subframe_end : 3;
        uint32_t loop_subframe_skip : 3;
        uint32_t subframe_decode_count : 4;
        uint32_t output_buffer_padding : 3;
        uint32_t sample_rate : 2;
        uint32_t is_stereo : 1;
        uint32_t unk_dword_1_c : 1;
        uint32_t output_buffer_valid : 1;
        // DWORD 2
        uint32_t input_buffer_read_offset : 26;
        uint32_t error_status : 5;
        uint32_t error_set : 1;
        // DWORD 3
        uint32_t loop_start : 26;
        uint32_t parser_error_status : 5;
        uint32_t parser_error_set : 1;
        // DWORD 4
        uint32_t loop_end : 26;
        uint32_t packet_metadata : 5;
        uint32_t current_buffer : 1;
        // DWORD 5-8
        uint32_t input_buffer_0_ptr;
        uint32_t input_buffer_1_ptr;
        uint32_t output_buffer_ptr;
        uint32_t work_buffer_ptr;
        // DWORD 9
        uint32_t output_buffer_read_offset : 5;
        uint32_t : 25;
        uint32_t stop_when_done : 1;
        uint32_t interrupt_when_done : 1;
        // DWORD 10-15
        uint32_t unk_dwords_10_15[6];
    };
    static_assert(sizeof(ContextData) == 64);

    struct Context
    {
        std::mutex lock;
        bool allocated = false;
        std::atomic<bool> enabled{ false }; // read by the worker without the lock
        int remainingSubframes = 0; // of the current fake frame

        // Real decoding (FFmpeg): decoded big-endian PCM waiting for output ring space.
        std::unique_ptr<ff::XmaDecoder> decoder;
        std::vector<uint8_t> pending;
        size_t pendingPos = 0;
        // Input position after our last update; anything else means the title moved it.
        uint32_t lastReadOffset = ~0u, lastBuffer = ~0u;

        void Reset()
        {
            if (decoder)
                decoder->Flush();
            pending.clear();
            pendingPos = 0;
            lastReadOffset = lastBuffer = ~0u;
            remainingSubframes = 0;
        }
    };

    uint32_t s_arrayVirt = 0, s_arrayPhys = 0;
    Context s_contexts[kContextCount];
    std::mutex s_allocLock;
    std::atomic<uint32_t> s_nextContextIndex{ 1 };

    void StoreRegister(uint32_t index, uint32_t value)
    {
        // Registers are little-endian (the title uses lwbrx/stwbrx).
        memcpy(g_base + kRegisterBase + index * 4, &value, 4);
    }

    ContextData Read(uint32_t id)
    {
        ContextData d;
        uint32_t* w = reinterpret_cast<uint32_t*>(&d);
        for (uint32_t i = 0; i < 16; i++)
            w[i] = Load32(s_arrayVirt + id * 64 + i * 4);
        return d;
    }

    void Write(uint32_t id, const ContextData& d)
    {
        const uint32_t* w = reinterpret_cast<const uint32_t*>(&d);
        for (uint32_t i = 0; i < 16; i++)
            Store32(s_arrayVirt + id * 64 + i * 4, w[i]);
    }

    uint32_t PacketCount(const ContextData& d, uint32_t buffer)
    {
        return buffer == 0 ? d.input_buffer_0_packet_count : d.input_buffer_1_packet_count;
    }

    bool AnyInputValid(const ContextData& d) { return d.input_buffer_0_valid || d.input_buffer_1_valid; }

    void SwapInputBuffer(ContextData& d)
    {
        if (d.current_buffer == 0)
            d.input_buffer_0_valid = 0;
        else
            d.input_buffer_1_valid = 0;
        d.current_buffer ^= 1;
        d.input_buffer_read_offset = kBitsPerPacketHeader;
    }

    // Advances through one 2 KB input packet and queues one frame of silence.
    void ProcessPacket(Context& c, ContextData& d)
    {
        if (!AnyInputValid(d) || c.remainingSubframes > 0)
            return;
        if (d.loop_count)
        {
            uint32_t loopStart = std::max(kBitsPerPacketHeader, uint32_t(d.loop_start));
            uint32_t loopEnd = std::max(kBitsPerPacketHeader, uint32_t(d.loop_end));
            if (d.input_buffer_read_offset == loopEnd)
            {
                d.input_buffer_read_offset = loopStart;
                if (d.loop_count != 255)
                    d.loop_count--;
            }
        }
        uint32_t packets = PacketCount(d, d.current_buffer);
        uint32_t packet = d.input_buffer_read_offset / kBitsPerPacket;
        if (packet >= packets)
        {
            SwapInputBuffer(d);
            return;
        }
        if (packet + 1 >= packets)
            SwapInputBuffer(d);
        else
            d.input_buffer_read_offset = (packet + 1) * kBitsPerPacket + kBitsPerPacketHeader;
        c.remainingSubframes = 4 << d.is_stereo;
    }

    constexpr int kSampleRates[] = { 24000, 32000, 44100, 48000 };
    std::atomic<uint64_t> s_packetsDecoded, s_samplesDecoded, s_decodeErrors;

    uint8_t* PhysicalPtr(uint32_t address) { return g_base + (0xA0000000 | mem::ToPhysical(address)); }

    // Real decoding: follows the context's packets (packet skip counts chain them
    // through buffers shared with other streams), honours loops and streaming buffer
    // swaps, and writes big-endian 16-bit PCM (stereo interleaved) into the output
    // ring as space allows. Positions are tracked per packet.
    void WorkDecode(Context& c, ContextData& d)
    {
        int rate = kSampleRates[d.sample_rate];
        int channels = d.is_stereo ? 2 : 1;
        if (!c.decoder || c.decoder->SampleRate() != rate || c.decoder->Channels() != channels)
        {
            c.decoder = std::make_unique<ff::XmaDecoder>(rate, channels);
            c.pending.clear();
            c.pendingPos = 0;
        }
        if (d.input_buffer_read_offset != c.lastReadOffset || d.current_buffer != c.lastBuffer)
        {
            // The title (re)positioned the stream: drop what was decoded for the old position.
            if (c.lastReadOffset != ~0u)
                c.decoder->Flush();
            c.pending.clear();
            c.pendingPos = 0;
        }

        uint32_t capacity = d.output_buffer_block_count;
        uint32_t readOffset = d.output_buffer_read_offset, writeOffset = d.output_buffer_write_offset;
        uint32_t freeBlocks = readOffset > writeOffset ? readOffset - writeOffset : capacity - writeOffset + readOffset;
        const uint32_t unit = 1u << d.is_stereo; // blocks per subframe
        uint8_t* out = PhysicalPtr(d.output_buffer_ptr);
        bool wrote = false;

        for (int guard = 0; guard < 64; guard++)
        {
            // Drain decoded samples into the ring.
            while (c.pendingPos < c.pending.size() && freeBlocks >= unit)
            {
                memcpy(out + writeOffset * kOutputBytesPerBlock, c.pending.data() + c.pendingPos, kOutputBytesPerBlock);
                c.pendingPos += kOutputBytesPerBlock;
                writeOffset = (writeOffset + 1) % capacity;
                freeBlocks--;
                wrote = true;
            }
            if (c.pendingPos < c.pending.size())
                break; // ring full
            c.pending.clear();
            c.pendingPos = 0;

            // Decode the next packet of this stream.
            if (!AnyInputValid(d))
                break;
            bool currentValid = d.current_buffer ? d.input_buffer_1_valid : d.input_buffer_0_valid;
            if (!currentValid)
            {
                d.current_buffer ^= 1;
                d.input_buffer_read_offset = kBitsPerPacketHeader;
                continue;
            }
            uint32_t packets = PacketCount(d, d.current_buffer);
            uint32_t offset = std::max(kBitsPerPacketHeader, uint32_t(d.input_buffer_read_offset));
            uint32_t packet = offset / kBitsPerPacket;
            if (packet >= packets)
            {
                SwapInputBuffer(d);
                continue;
            }
            const uint8_t* data = PhysicalPtr(d.current_buffer ? d.input_buffer_1_ptr : d.input_buffer_0_ptr) + packet * kBytesPerPacket;
            s_packetsDecoded++;
            bool ok = c.decoder->DecodePacket(data, [&](const float* const* planes, int count) {
                size_t base = c.pending.size();
                c.pending.resize(base + size_t(count) * channels * 2);
                uint8_t* o = c.pending.data() + base;
                for (int i = 0; i < count; i++)
                    for (int ch = 0; ch < channels; ch++)
                    {
                        float v = planes[ch][i] * 32767.0f;
                        int s = int(v < -32768.0f ? -32768.0f : v > 32767.0f ? 32767.0f : v);
                        *o++ = uint8_t(s >> 8);
                        *o++ = uint8_t(s);
                    }
                s_samplesDecoded += uint32_t(count);
            });
            if (!ok)
                s_decodeErrors++;
            // Keep whole output blocks; a partial tail waits for the next packet.
            uint32_t next = packet + 1 + data[3];
            uint32_t nextOffset = next * kBitsPerPacket + kBitsPerPacketHeader;
            bool looping = d.loop_count && d.loop_end > d.loop_start;
            if (looping && (next >= packets || nextOffset >= d.loop_end))
            {
                d.input_buffer_read_offset = std::max(kBitsPerPacketHeader, uint32_t(d.loop_start));
                if (d.loop_count != 255)
                    d.loop_count--;
                c.decoder->Flush();
            }
            else if (next >= packets)
                SwapInputBuffer(d); // streaming continues in the other buffer without a flush
            else
                d.input_buffer_read_offset = nextOffset;
            wrote = true;
            size_t whole = c.pending.size() / kOutputBytesPerBlock * kOutputBytesPerBlock;
            if (whole != c.pending.size())
            {
                // Rare (frames are 512 samples = whole blocks); pad with silence.
                c.pending.resize(whole + kOutputBytesPerBlock, 0);
            }
        }

        if (!wrote)
            return;
        d.output_buffer_write_offset = writeOffset;
        if (writeOffset == readOffset && freeBlocks == 0)
            d.output_buffer_valid = 0; // full: the title marks it valid again after reading
        c.lastReadOffset = d.input_buffer_read_offset;
        c.lastBuffer = d.current_buffer;
    }

    // Decodes into the context's output ring while it is enabled. As on hardware a
    // kicked context stays enabled - decoding resumes whenever the title frees
    // output space - until the title locks it.
    void Work(uint32_t id)
    {
        Context& c = s_contexts[id];
        std::lock_guard lock(c.lock);
        if (!c.enabled || !c.allocated)
            return;

        ContextData d = Read(id);
        if (!d.output_buffer_valid || !d.output_buffer_block_count)
            return;
        if (ff::Available())
        {
            ContextData before = d;
            WorkDecode(c, d);
            if (memcmp(&before, &d, sizeof(d)) != 0)
                Write(id, d);
            return;
        }

        // Output ring buffer in 256-byte blocks.
        uint32_t capacity = d.output_buffer_block_count;
        uint32_t readOffset = d.output_buffer_read_offset, writeOffset = d.output_buffer_write_offset;
        if (!capacity)
            return;
        auto freeBlocks = [&] {
            return readOffset > writeOffset ? readOffset - writeOffset
                 : capacity - writeOffset + readOffset; // equal = empty: whole buffer writable
        };
        int remaining = int(freeBlocks());
        int minimum = int(d.subframe_decode_count) * 2 - 1;
        uint32_t outVirt = 0xA0000000 | mem::ToPhysical(d.output_buffer_ptr);
        bool wrote = false;

        while (remaining >= minimum && remaining > 0)
        {
            ProcessPacket(c, d);
            if (c.remainingSubframes)
            {
                int n = std::min({ c.remainingSubframes, int(std::max(1u, uint32_t(d.subframe_decode_count))), remaining });
                for (int i = 0; i < n; i++)
                {
                    memset(g_base + outVirt + writeOffset * kOutputBytesPerBlock, 0, kOutputBytesPerBlock);
                    writeOffset = (writeOffset + 1) % capacity;
                }
                remaining -= n;
                c.remainingSubframes -= n;
                wrote = true;
            }
            if (!AnyInputValid(d) || d.error_status == 4)
                break;
            if (!c.remainingSubframes && !AnyInputValid(d))
                break;
        }
        if (!wrote)
            return; // nothing changed: leave the title's view of the context alone
        d.output_buffer_write_offset = writeOffset;
        // A full ring (write caught up with read) is handed back to the title, which
        // marks it valid again once it has consumed some of it (as Xenia does).
        if (writeOffset == readOffset)
            d.output_buffer_valid = 0;
        Write(id, d);
    }

    // The decoder runs on its own, servicing every enabled context.
    std::atomic<bool> s_workerStarted{ false };
    void StartWorker()
    {
        if (s_workerStarted.exchange(true))
            return;
        std::thread([] {
            SetThreadDescription(GetCurrentThread(), L"XMA decoder");
            auto lastLog = std::chrono::steady_clock::now();
            for (;;)
            {
                uint32_t active = 0;
                for (uint32_t id = 0; id < kContextCount; id++)
                    if (s_contexts[id].enabled)
                    {
                        active++;
                        Work(id);
                    }
                if (std::chrono::steady_clock::now() - lastLog > std::chrono::seconds(10))
                {
                    lastLog = std::chrono::steady_clock::now();
                    uint64_t submitted, underruns, dropped;
                    audio::TakeOutputStats(submitted, underruns, dropped);
                    LOGI("xma: %u contexts enabled; %llu packets, %llu samples decoded, %llu errors; output %llu frames, "
                         "%llu underruns, %llu dropped",
                         active, (unsigned long long)s_packetsDecoded.load(), (unsigned long long)s_samplesDecoded.load(),
                         (unsigned long long)s_decodeErrors.load(), (unsigned long long)submitted,
                         (unsigned long long)underruns, (unsigned long long)dropped);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
    }

    void Clear(uint32_t id)
    {
        Context& c = s_contexts[id];
        std::lock_guard lock(c.lock);
        ContextData d = Read(id);
        d.input_buffer_0_valid = 0;
        d.input_buffer_1_valid = 0;
        d.output_buffer_valid = 0;
        d.output_buffer_read_offset = 0;
        d.output_buffer_write_offset = 0;
        d.input_buffer_read_offset = kBitsPerPacketHeader;
        c.Reset();
        Write(id, d);
    }

    template <typename F>
    void ForEachBit(uint32_t group, uint32_t bits, F&& f)
    {
        while (bits)
        {
            uint32_t id = group * 32 + uint32_t(__builtin_ctz(bits));
            if (id < kContextCount)
                f(id);
            bits &= bits - 1;
        }
    }
}

namespace apu
{
    void XmaInit()
    {
        s_arrayVirt = mem::PhysicalAlloc(kContextCount * 64, 0x1000, 0x1000);
        s_arrayPhys = mem::ToPhysical(s_arrayVirt);
        StoreRegister(kRegContextArrayAddress, s_arrayPhys);
        StoreRegister(kRegCurrentContextIndex, 0);
        LOGI("xma: %u contexts at %08X (physical %08X)", kContextCount, s_arrayVirt, s_arrayPhys);
    }
}

// ---- mid-asm hooks (config/MCLA.toml) -------------------------------------------

// After `stwbrx r5, 0, r4` to a kick/lock/clear register.
void XmaRegisterWriteHook(PPCRegister& r4, PPCRegister& r5)
{
    uint32_t reg = (r4.u32 - kRegisterBase) / 4;
    uint32_t value = r5.u32;
    if (reg >= kRegKick && reg < kRegKick + 10)
    {
        StartWorker();
        ForEachBit(reg - kRegKick, value, [](uint32_t id) {
            {
                std::lock_guard lock(s_contexts[id].lock);
                s_contexts[id].enabled = true;
            }
            Work(id);
        });
    }
    else if (reg >= kRegLock && reg < kRegLock + 10)
    {
        ForEachBit(reg - kRegLock, value, [](uint32_t id) {
            std::lock_guard lock(s_contexts[id].lock);
            s_contexts[id].enabled = false;
        });
    }
    else if (reg >= kRegClear && reg < kRegClear + 10)
        ForEachBit(reg - kRegClear, value, [](uint32_t id) { Clear(id); });
    else
        LOGI("xma: write to unexpected register %03X = %08X", reg, value);
}

// Before `lwbrx` of CurrentContextIndex: present a rotating context index.
void XmaCurrentContextReadHook()
{
    uint32_t next = s_nextContextIndex.fetch_add(1) % kContextCount;
    StoreRegister(kRegCurrentContextIndex, next);
}

// ---- kernel imports ---------------------------------------------------------------

GUEST_IMPORT(XMACreateContext)
{
    TRACE("XMACreateContext");
    std::lock_guard lock(s_allocLock);
    for (uint32_t id = 0; id < kContextCount; id++)
    {
        if (!s_contexts[id].allocated)
        {
            std::lock_guard contextLock(s_contexts[id].lock);
            s_contexts[id].allocated = true;
            s_contexts[id].enabled = false;
            s_contexts[id].Reset();
            memset(g_base + s_arrayVirt + id * 64, 0, 64);
            Store32(ctx.r3.u32, s_arrayVirt + id * 64);
            ctx.r3.u64 = xstatus::Success;
            return;
        }
    }
    LOGI("xma: out of contexts");
    Store32(ctx.r3.u32, 0);
    ctx.r3.u64 = xstatus::NoMemory;
}

GUEST_IMPORT(XMAReleaseContext)
{
    TRACE("XMAReleaseContext");
    uint32_t id = (ctx.r3.u32 - s_arrayVirt) / 64;
    if (id < kContextCount)
    {
        std::lock_guard lock(s_contexts[id].lock);
        s_contexts[id].allocated = false;
        s_contexts[id].enabled = false;
        s_contexts[id].Reset();
        memset(g_base + s_arrayVirt + id * 64, 0, 64);
    }
    ctx.r3.u64 = 0;
}
