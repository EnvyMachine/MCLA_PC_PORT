// Guest textures as host textures.
//
// A texture is identified by the fetch constant fields that describe its memory
// (addresses, format, size, tiling, mips). Its data is untiled, endian-swapped
// and converted on the CPU straight into upload memory, and the guest pages it
// came from are watched (runtime/memory_watch.h): after the guest writes one,
// the texture is reloaded on its next use. Views (the fetch constant's swizzle
// and signedness) and samplers are created on demand.
#pragma once

#include <cstdint>

namespace render
{
    void InitTextureCache();

    // View heap descriptor for the texture in a fetch constant (6 dwords, host
    // order) that a shader samples as fetchDimension (the tfetch dimension:
    // 0 1D, 1 2D, 2 3D/stacked, 3 cube). Bits 16-29 carry the conversions the
    // shader applies after sampling (PostFetch in xenos/common.hlsli). Returns
    // 0 when there is nothing usable to bind; the caller binds a placeholder.
    uint32_t GetTextureDescriptor(const uint32_t fetch[6], uint32_t fetchDimension);

    // Sampler heap descriptor for the fetch constant's filtering and addressing.
    uint32_t GetSamplerDescriptor(const uint32_t fetch[6]);

    // Once per frame: frees textures that have not been used for a while.
    void TextureCacheEndFrame();

    // Bytes of texture data loaded since the last call.
    uint64_t TakeTextureUploadBytes();

    // Hitch diagnosis: texture loading this frame.
    double TextureLoadMsThisFrame();
    uint64_t TextureUploadBytesThisFrame();
    void ResetTextureFrameStats();
}
