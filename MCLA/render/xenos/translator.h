// Xenos shader microcode -> HLSL (Shader Model 6), for the D3D12 renderer.
//
// Derived from XenosRecomp's ShaderRecompiler (MIT License, Copyright (c) 2025
// hedge-dev and contributors). Changes for this runtime:
// - constants are read from the guest constant register file by index
//   (g_VS / g_PS) instead of by constant-table name;
// - vertex fetches read guest memory directly using the format, stride and
//   offset of the *patched* program D3D actually loaded (no input layouts);
// - texture fetches are bound by fetch-constant slot and support explicit
//   LOD/gradients (setTexLOD, setGradientH/V, getGradients, getCompTexLOD);
// - bool and loop constants come from the register file;
// - the guest viewport transform and alpha test are driven by per-draw
//   system constants.
#pragma once

#include <cstdint>
#include <string>

namespace render::xenos
{
    struct Translation
    {
        std::string hlsl;
        bool isPixel = false;
        uint32_t textureSlots = 0;  // fetch slots read by texture fetches
        uint8_t textureDims[32] = {}; // TextureDimension per used slot
        uint32_t colorOutputs = 0;  // pixel shaders: oC0-oC3 written (bits)
        bool writesDepth = false;
        uint32_t vertexFetchSlots[3] = {}; // vertex fetch constants (0-95) read
        std::string error;
    };

    // container: the D3D shader container (big-endian, as in guest memory).
    // program: the microcode the GPU loads (big-endian dwords; for vertex shaders
    // the copy D3D patched for the current vertex declaration).
    // linkage (vertex shaders only): the semantics of the pixel shader the program
    // was linked to, when they differ from the container's own export order.
    struct Linkage;
    bool Translate(const uint8_t* container, size_t containerSize, const uint8_t* program, uint32_t programDwords,
                   Translation& out, const Linkage* linkage = nullptr);

    // Semantics of interpolator N (vertex shaders: export N; pixel shaders: the
    // register loaded from interpolator N), as a container declares them.
    struct Linkage
    {
        bool valid[16];
        uint8_t usage[16];      // DeclUsage
        uint8_t usageIndex[16];
    };
    bool ContainerLinkage(const uint8_t* container, size_t containerSize, Linkage& out);

    // Microcode with no container (Direct3D's built-in shaders and per-pixel-shader
    // patched vertex shader copies, loaded inline from command buffers). The hardware
    // links export N to pixel shader register N; translated shaders link by semantic,
    // so interpolator N gets the partner stage's semantic from `linkage` (TEXCOORDN
    // without one). Pixel shader outputs are the ones the program writes.
    bool TranslateRaw(bool isPixel, const uint8_t* program, uint32_t programDwords, Translation& out,
                      const Linkage* linkage = nullptr);

    // The support header prepended to every translated shader.
    const char* CommonHlsl();
}
