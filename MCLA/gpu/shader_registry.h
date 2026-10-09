// Guest shaders, captured when the game creates them.
//
// D3D's shader-creation functions take a complete shader container: the virtual
// part (header, constant table, vertex elements, interpolators) followed by the
// physical part (Xenos microcode). D3D copies the microcode into physical memory
// and the command stream later loads it by physical address (PM4 IM_LOAD), so the
// registry maps that address back to the container XenosRecomp needs.
#pragma once

#include <cstdint>
#include <vector>

namespace gpu
{
    struct GuestShader
    {
        uint64_t hash = 0;              // XXH3-64 of the container (same key XenosRecomp uses)
        bool isPixel = false;
        std::vector<uint8_t> container; // big-endian, as in guest memory; empty for raw microcode
        uint32_t microcodePhys = 0;     // physical address of the loaded microcode
        void* host = nullptr;           // renderer's translation, filled lazily
    };

    // Called from the mid-asm hooks after a create call returns.
    void RegisterShader(uint32_t containerAddress, uint32_t shaderObject);

    // Lookups take the stage being loaded: trivial programs (a single export of
    // r0) have the same microcode as a vertex and as a pixel shader.

    // The shader whose microcode lives at this physical address, or null.
    GuestShader* FindShaderByMicrocode(uint32_t physAddress, bool isPixel);

    // D3D re-emits vertex shaders inline (PM4 IM_LOAD_IMMEDIATE) with their vertex
    // fetches patched for the current vertex declaration (sub_82424320); each copy
    // is recorded by its physical address in the command buffer.
    void RegisterInlineCopy(uint32_t copyAddress, uint32_t sourceAddress);
    GuestShader* FindShaderByInlineCopy(uint32_t physAddress, bool isPixel);

    // Fallback: the shader whose program matches the loaded microcode, ignoring
    // vertex fetch instructions (copies from paths the hooks don't see).
    GuestShader* FindShaderByProgram(uint32_t codeAddress, uint32_t dwords, bool isPixel);

    // Microcode no container was ever created for (Direct3D's built-in shaders,
    // which command buffers load inline): a shader with an empty container, one
    // per distinct program.
    GuestShader* GetRawShader(uint32_t codeAddress, uint32_t dwords, bool isPixel);

    size_t ShaderCount();
}
