// GPU mirror of guest physical memory (512 MB), the source for vertex fetches,
// texture loads and index buffers. Pages are copied in on first use and again
// after the CPU writes them (runtime/memory_watch.h).
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace render
{
    void InitSharedMemory();

    // Makes guest physical [phys, phys+size) current before the next GPU work that
    // reads the mirror; the copies are recorded by CommitSharedMemoryUploads.
    void UseSharedMemory(uint32_t phys, uint32_t size);

    // Records pending page copies (and the state transitions around them) into
    // the command list. Leaves the mirror readable by shaders.
    bool CommitSharedMemoryUploads(ID3D12GraphicsCommandList* list); // true if it recorded copies

    ID3D12Resource* SharedMemoryBuffer();

    // Bytes uploaded since the last call (statistics).
    uint64_t TakeSharedMemoryUploadBytes();

    // Hitch diagnosis: this frame's page protection and copy time, runs and bytes.
    void TakeSharedMemoryFrameStats(double& protectMs, double& copyMs, uint32_t& runs, uint64_t& bytes);
}
