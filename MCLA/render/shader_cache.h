// Host shaders: guest programs translated to HLSL (render/xenos/translator.h)
// and compiled to DXIL with DXC on worker threads. Compiled blobs are kept in
// <exe dir>/shader_cache so later runs start without compiling.
#pragma once

#include "xenos/translator.h"

#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace gpu { struct GuestShader; }

namespace render
{
    struct HostShader
    {
        enum State : uint32_t { kPending, kReady, kFailed };
        std::atomic<uint32_t> state{ kPending };
        uint64_t key = 0;
        std::vector<uint8_t> dxil;
        // Translation metadata (valid once ready).
        bool isPixel = false;
        uint32_t textureSlots = 0;
        uint8_t textureDims[32] = {};
        uint32_t colorOutputs = 0;
        bool writesDepth = false;
        uint32_t vertexFetchSlots[3] = {}; // vertex fetch constants (0-95) read

        D3D12_SHADER_BYTECODE Bytecode() const { return { dxil.data(), dxil.size() }; }
    };

    bool InitShaderCache();

    // Compiles a runtime-provided HLSL source synchronously (built-in shaders).
    bool CompileHlsl(const char* source, const wchar_t* target, std::vector<uint8_t>& dxil);

    // The host shader for a guest shader loaded with this microcode (the program
    // as the GPU sees it), drawn together with `partner` (the other stage, whose
    // interpolator semantics raw microcode links to). Returns null if the guest
    // shader is unknown; otherwise an entry that may still be compiling.
    HostShader* GetHostShader(const gpu::GuestShader* guest, uint32_t codeAddress, uint32_t dwords,
                              const gpu::GuestShader* partner);

    // Statistics.
    uint32_t PendingShaderCompiles();

    // Development: translates and compiles every container in a directory of
    // shader dumps (MCLA_DUMP_SHADERS) and reports failures. Returns the number
    // of failures.
    int TestShaderDirectory(const wchar_t* directory);
}
