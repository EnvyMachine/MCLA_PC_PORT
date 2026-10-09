// Per-draw constants every translated shader can read (HLSL: cbuffer
// SystemConstants, register b2, in render/xenos/common.hlsli; keep in sync).
#pragma once

#include <cstdint>

namespace render::xenos
{
    enum SystemFlags : uint32_t
    {
        kFlagXYNotDividedByW = 1u << 0, // PA_CL_VTE_CNTL.vtx_xy_fmt
        kFlagZNotDividedByW = 1u << 1,  // PA_CL_VTE_CNTL.vtx_z_fmt
        kFlagWIsReciprocal = 1u << 2,   // PA_CL_VTE_CNTL.vtx_w0_fmt
        // Bits 8-11: color output i goes into a 2_10_10_10_FLOAT target, which keeps
        // 7e3 color (0..31.875) and alpha in 0..1.
        kFlagColor7e3 = 1u << 8,
        // Bits 12-15: ... and is written unblended, so its 2-bit alpha is stored as is.
        kFlagColor7e3Unblended = 1u << 12,
    };

    struct SystemConstants
    {
        uint32_t fetch[192];       // fetch constants (registers 0x4800-0x48BF)
        uint32_t bools[8];         // bool constants (0x4900-0x4907)
        uint32_t loops[32];        // loop constants (0x4908-0x4927)
        uint32_t textureIndex[32]; // bindless SRV index per fetch slot
        uint32_t samplerIndex[32]; // sampler heap index per fetch slot
        float ndcScale[4];         // guest clip space -> host clip space
        float ndcOffset[4];
        uint32_t vertexIndexEndian;
        uint32_t vertexIndexOffset; // VGT_INDX_OFFSET
        uint32_t vertexIndexMin;    // VGT_MIN_VTX_INDX
        uint32_t vertexIndexMax;    // VGT_MAX_VTX_INDX
        uint32_t flags;             // SystemFlags
        uint32_t alphaTestFunc;     // CompareFunction 0-7; 7 = always (disabled)
        float alphaRef;
        uint32_t pad;
        // Samples per guest pixel in the render target (render targets are EDRAM
        // sample grids: 2x MSAA 1x2, 4x MSAA 2x2), for pixel positions.
        float pixelScale[4];
        // Per render target: 2^RB_COLOR_INFO.color_exp_bias, which the hardware applies
        // to the shader's color output (all four components, before blending).
        float colorExpBias[4];
    };
    static_assert(sizeof(SystemConstants) == 1280);
}
