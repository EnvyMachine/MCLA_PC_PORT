#include "descriptors.h"
#include "device.h"
#include "../runtime/log.h"

#include <vector>

namespace render
{
    namespace
    {
        struct Heap
        {
            ComPtr<ID3D12DescriptorHeap> heap;
            uint32_t increment = 0;
            uint32_t capacity = 0;
            uint32_t next = 0;
            // Freed indices with the frame that last could use them: reusable once
            // that frame has completed on the GPU.
            std::vector<std::pair<uint64_t, uint32_t>> freeList;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        };

        Heap s_heaps[4];

        void Create(HeapKind kind, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, bool shaderVisible, uint32_t reserved)
        {
            Heap& h = s_heaps[int(kind)];
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = type;
            desc.NumDescriptors = capacity;
            desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(Device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&h.heap))))
            {
                LOGI("render: could not create a descriptor heap (type %d, %u)", int(type), capacity);
                LogFlush();
                ExitProcess(1);
            }
            h.increment = Device()->GetDescriptorHandleIncrementSize(type);
            h.capacity = capacity;
            h.next = reserved;
            h.cpu = h.heap->GetCPUDescriptorHandleForHeapStart();
            if (shaderVisible)
                h.gpu = h.heap->GetGPUDescriptorHandleForHeapStart();
        }
    }

    void InitDescriptors()
    {
        // View heap: indices 0-2 are the placeholder 2D/3D/cube textures.
        Create(HeapKind::kView, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536, true, 3);
        // Sampler heap: index 0 is the placeholder sampler.
        Create(HeapKind::kSampler, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048, true, 1);
        Create(HeapKind::kRtv, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4096, false, 0);
        Create(HeapKind::kDsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1024, false, 0);
    }

    uint32_t AllocateDescriptor(HeapKind kind)
    {
        Heap& h = s_heaps[int(kind)];
        if (!h.freeList.empty() && h.freeList.front().first <= CompletedFrame())
        {
            uint32_t index = h.freeList.front().second;
            h.freeList.erase(h.freeList.begin());
            return index;
        }
        if (h.next >= h.capacity)
        {
            LOGI("render: descriptor heap %d is full (%u)", int(kind), h.capacity);
            LogFlush();
            ExitProcess(1);
        }
        return h.next++;
    }

    void FreeDescriptor(HeapKind kind, uint32_t index)
    {
        s_heaps[int(kind)].freeList.emplace_back(FrameNumber(), index);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(HeapKind kind, uint32_t index)
    {
        const Heap& h = s_heaps[int(kind)];
        return { h.cpu.ptr + size_t(index) * h.increment };
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(HeapKind kind, uint32_t index)
    {
        const Heap& h = s_heaps[int(kind)];
        return { h.gpu.ptr + uint64_t(index) * h.increment };
    }

    ID3D12DescriptorHeap* ViewHeap() { return s_heaps[int(HeapKind::kView)].heap.Get(); }
    ID3D12DescriptorHeap* SamplerHeap() { return s_heaps[int(HeapKind::kSampler)].heap.Get(); }
}
