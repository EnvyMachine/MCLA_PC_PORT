// Xenos shader microcode helpers.
#pragma once

#include <cstdint>

namespace gpu
{
    // Hash of a shader program (big-endian dwords, as in guest memory) that ignores
    // its vertex fetch instructions: D3D patches those for the current vertex
    // declaration, and everything else stays identical to the container's copy.
    uint64_t MicrocodeHashIgnoringVertexFetches(const uint8_t* code, uint32_t dwords);
}
