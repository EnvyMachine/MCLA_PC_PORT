// Translates the Xenos draw stream into D3D12 work (see notes/RENDERER.md).
// Called on the GPU command-processor thread.
#pragma once

#include <cstdint>

namespace gpu { struct GuestShader; }

namespace render
{
    struct DrawPacket
    {
        uint32_t initiator = 0;  // VGT_DRAW_INITIATOR
        uint32_t indexBase = 0;  // VGT_DMA_BASE (physical)
        uint32_t indexSize = 0;  // VGT_DMA_SIZE
        const gpu::GuestShader* vs = nullptr;
        uint32_t vsCode = 0, vsDwords = 0; // microcode as loaded (guest address)
        const gpu::GuestShader* ps = nullptr;
        uint32_t psCode = 0, psDwords = 0;
    };

    bool InitRenderer();

    // A draw packet: an ordinary draw, or a resolve (EDRAM copy) when
    // RB_MODECONTROL selects copy mode.
    void Draw(const DrawPacket& packet);

    // PM4 XE_SWAP: present the front buffer resolved at frontBufferPhys.
    void Swap(uint32_t frontBufferPhys, uint32_t width, uint32_t height);

    // True during the frame MCLA_DRAW_LOG selects (detailed logging).
    bool LoggingFrame();
}
