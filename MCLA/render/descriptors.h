// Descriptor heaps: one shader-visible CBV/SRV/UAV heap used bindlessly (shaders
// index it by the slot numbers in their system constants), one shader-visible
// sampler heap, and CPU-only heaps for render target and depth views.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace render
{
    enum class HeapKind { kView, kSampler, kRtv, kDsv };

    void InitDescriptors();

    // Persistent allocation (index into the heap); never returns 0 for kView
    // and kSampler (index 0 holds placeholders).
    uint32_t AllocateDescriptor(HeapKind kind);
    void FreeDescriptor(HeapKind kind, uint32_t index);

    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(HeapKind kind, uint32_t index);
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(HeapKind kind, uint32_t index);

    ID3D12DescriptorHeap* ViewHeap();
    ID3D12DescriptorHeap* SamplerHeap();
}
