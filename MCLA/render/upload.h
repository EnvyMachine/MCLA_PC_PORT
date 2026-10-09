// CPU-to-GPU upload memory: persistently mapped chunks in an UPLOAD heap, handed
// out linearly. A chunk is reused once every frame that used it has completed.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace render
{
    struct UploadSpace
    {
        uint8_t* cpu = nullptr;
        ID3D12Resource* resource = nullptr;
        uint64_t offset = 0; // within resource
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    };

    UploadSpace AllocateUpload(uint64_t size, uint64_t alignment = 256);
}
