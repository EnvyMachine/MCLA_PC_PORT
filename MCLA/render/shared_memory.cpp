#include "shared_memory.h"
#include "device.h"
#include "upload.h"
#include "../runtime/guest.h"
#include "../runtime/log.h"
#include "../runtime/memory_watch.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace render
{
    namespace
    {
        constexpr uint64_t kSize = 0x20000000;
        constexpr D3D12_RESOURCE_STATES kReadState =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_COPY_SOURCE;

        ComPtr<ID3D12Resource> s_buffer;

        struct PendingCopy
        {
            ID3D12Resource* source;
            uint64_t sourceOffset;
            uint32_t phys;
            uint32_t size;
        };
        std::vector<PendingCopy> s_pending;
        uint64_t s_uploadBytes = 0;
        // Hitch diagnosis: this frame's mirror work.
        int64_t s_frameProtectTicks = 0, s_frameCopyTicks = 0;
        uint32_t s_frameRuns = 0;
        uint64_t s_frameBytes = 0;

        void QueueRun(uint32_t firstPage, uint32_t count)
        {
            LARGE_INTEGER t0, t1, t2;
            QueryPerformanceCounter(&t0);
            // Protect first: a CPU write racing with the copy below faults and
            // un-mirrors the page again, so it is re-uploaded next time.
            mem::MarkPagesMirrored(firstPage, count);
            QueryPerformanceCounter(&t1);
            uint32_t phys = firstPage << mem::kWatchPageShift;
            uint32_t size = count << mem::kWatchPageShift;
            UploadSpace space = AllocateUpload(size, 256);
            memcpy(space.cpu, g_base + 0xA0000000 + phys, size);
            QueryPerformanceCounter(&t2);
            s_pending.push_back({ space.resource, space.offset, phys, size });
            s_uploadBytes += size;
            s_frameProtectTicks += t1.QuadPart - t0.QuadPart;
            s_frameCopyTicks += t2.QuadPart - t1.QuadPart;
            s_frameRuns++;
            s_frameBytes += size;
        }
    }

    void InitSharedMemory()
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = kSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kReadState, nullptr,
                                                     IID_PPV_ARGS(&s_buffer))))
        {
            LOGI("render: could not create the 512 MB guest memory mirror");
            LogFlush();
            ExitProcess(1);
        }
        s_buffer->SetName(L"guest physical memory");
    }

    void UseSharedMemory(uint32_t phys, uint32_t size)
    {
        if (!size)
            return;
        phys &= 0x1FFFFFFF;
        uint64_t end = std::min<uint64_t>(uint64_t(phys) + size, kSize);
        uint32_t first = phys >> mem::kWatchPageShift;
        uint32_t last = uint32_t((end - 1) >> mem::kWatchPageShift);
        // MCLA_NO_MIRROR=1: upload everything a draw reads, every time (debugging).
        static const bool noMirror = [] {
            char v[4];
            return GetEnvironmentVariableA("MCLA_NO_MIRROR", v, sizeof(v)) > 0;
        }();
        uint32_t runStart = 0, runCount = 0;
        for (uint32_t page = first; page <= last; page++)
        {
            if (noMirror || !mem::IsPageMirrored(page))
            {
                if (!runCount)
                    runStart = page;
                runCount++;
            }
            else if (runCount)
            {
                QueueRun(runStart, runCount);
                runCount = 0;
            }
        }
        if (runCount)
            QueueRun(runStart, runCount);
    }

    bool CommitSharedMemoryUploads(ID3D12GraphicsCommandList* list)
    {
        if (s_pending.empty())
            return false;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = s_buffer.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = kReadState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &b);
        for (const PendingCopy& c : s_pending)
            list->CopyBufferRegion(s_buffer.Get(), c.phys, c.source, c.sourceOffset, c.size);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        s_pending.clear();
        return true;
    }

    ID3D12Resource* SharedMemoryBuffer() { return s_buffer.Get(); }

    void TakeSharedMemoryFrameStats(double& protectMs, double& copyMs, uint32_t& runs, uint64_t& bytes)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        protectMs = double(s_frameProtectTicks) * 1000.0 / double(f.QuadPart);
        copyMs = double(s_frameCopyTicks) * 1000.0 / double(f.QuadPart);
        runs = s_frameRuns;
        bytes = s_frameBytes;
        s_frameProtectTicks = s_frameCopyTicks = 0;
        s_frameRuns = 0;
        s_frameBytes = 0;
    }

    uint64_t TakeSharedMemoryUploadBytes()
    {
        uint64_t v = s_uploadBytes;
        s_uploadBytes = 0;
        return v;
    }
}
