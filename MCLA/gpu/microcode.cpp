#include "microcode.h"

#include <cstring>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace gpu
{
    namespace
    {
        uint32_t LoadBE(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

        // Control-flow opcodes that execute a clause of ALU/fetch instructions.
        bool IsExec(uint32_t opcode)
        {
            switch (opcode)
            {
            case 1: case 2:   // Exec, ExecEnd
            case 3: case 4:   // CondExec, CondExecEnd
            case 5: case 6:   // CondExecPred, CondExecPredEnd
            case 13: case 14: // CondExecPredClean, CondExecPredCleanEnd
                return true;
            default:
                return false;
            }
        }
    }

    uint64_t MicrocodeHashIgnoringVertexFetches(const uint8_t* code, uint32_t dwords)
    {
        std::vector<uint32_t> words(dwords);
        for (uint32_t i = 0; i < dwords; i++)
            words[i] = LoadBE(code + i * 4);

        // Control flow comes first, two 48-bit instructions per three dwords, and
        // ends where the lowest-addressed clause begins.
        uint32_t cfEnd = dwords;
        for (uint32_t at = 0; at + 3 <= cfEnd; at += 3)
        {
            const uint32_t lo[2] = { words[at], (words[at + 1] >> 16) | (words[at + 2] << 16) };
            const uint32_t hi[2] = { words[at + 1] & 0xFFFF, words[at + 2] >> 16 };
            for (int k = 0; k < 2; k++)
            {
                if (!IsExec((hi[k] >> 12) & 0xF))
                    continue;
                uint32_t address = lo[k] & 0xFFF, count = (lo[k] >> 12) & 7, sequence = (lo[k] >> 16) & 0xFFF;
                if (address * 3 < cfEnd)
                    cfEnd = address * 3;
                for (uint32_t i = 0; i < count; i++)
                {
                    uint32_t instr = (address + i) * 3;
                    bool isFetch = (sequence >> (i * 2)) & 1;
                    if (isFetch && instr + 3 <= dwords && (words[instr] & 0x1F) == 0) // vertex fetch
                        words[instr] = words[instr + 1] = words[instr + 2] = 0;
                }
            }
        }
        return XXH3_64bits(words.data(), words.size() * 4);
    }
}
