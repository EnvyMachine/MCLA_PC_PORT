// Xenos shader microcode and D3D shader container layouts.
// From XenosRecomp (MIT License, Copyright (c) 2025 hedge-dev and contributors):
// shader_code.h, shader.h and constant_table.h, merged.
#pragma once

#include <cstdint>
#include <type_traits>

namespace render::xenos
{
    template <typename T>
    struct be
    {
        T value;

        T get() const
        {
            using U = std::conditional_t<std::is_enum_v<T>, std::underlying_type<T>, std::type_identity<T>>::type;
            U v = U(value);
            if constexpr (sizeof(U) == 2)
                v = U(__builtin_bswap16(uint16_t(v)));
            else if constexpr (sizeof(U) == 4)
                v = U(__builtin_bswap32(uint32_t(v)));
            return T(v);
        }
        operator T() const { return get(); }
    };

    // ---- D3D shader container ----

    struct ShaderContainer
    {
        be<uint32_t> flags; // 0x102A1100 pixel, 0x102A1101 vertex
        be<uint32_t> virtualSize;
        be<uint32_t> physicalSize;
        be<uint32_t> fieldC;
        be<uint32_t> constantTableOffset;
        be<uint32_t> definitionTableOffset;
        be<uint32_t> shaderOffset;
        be<uint32_t> field1C;
        be<uint32_t> field20;
    };

    struct Shader
    {
        be<uint32_t> physicalOffset;
        be<uint32_t> size; // bytes
        be<uint32_t> field8;
        be<uint32_t> fieldC; // pixel position register: (fieldC >> 8) & 0xFF
        be<uint32_t> field10;
        be<uint32_t> interpolatorInfo; // interpolator count: (interpolatorInfo >> 5) & 0x1F
    };

    enum class DeclUsage : uint32_t
    {
        Position = 0, BlendWeight = 1, BlendIndices = 2, Normal = 3, PointSize = 4, TexCoord = 5, Tangent = 6,
        Binormal = 7, TessFactor = 8, PositionT = 9, Color = 10, Fog = 11, Depth = 12, Sample = 13
    };

    struct Interpolator
    {
        uint32_t usageIndex : 4;
        DeclUsage usage : 4;
        uint32_t reg : 4;
        uint32_t : 20;
    };

    struct VertexShader : Shader
    {
        be<uint32_t> field18;
        be<uint32_t> vertexElementCount;
        be<uint32_t> field20;
        be<uint32_t> vertexElementsAndInterpolators[]; // field18 + vertex elements + interpolators
    };

    enum PixelShaderOutputs : uint32_t
    {
        PIXEL_SHADER_OUTPUT_COLOR0 = 0x1,
        PIXEL_SHADER_OUTPUT_COLOR1 = 0x2,
        PIXEL_SHADER_OUTPUT_COLOR2 = 0x4,
        PIXEL_SHADER_OUTPUT_COLOR3 = 0x8,
        PIXEL_SHADER_OUTPUT_DEPTH = 0x10
    };

    struct PixelShader : Shader
    {
        be<uint32_t> field18;
        be<PixelShaderOutputs> outputs;
        be<uint32_t> interpolators[];
    };

    // ---- Microcode ----

    enum class ControlFlowOpcode : uint32_t
    {
        Nop = 0, Exec = 1, ExecEnd = 2, CondExec = 3, CondExecEnd = 4, CondExecPred = 5, CondExecPredEnd = 6,
        LoopStart = 7, LoopEnd = 8, CondCall = 9, Return = 10, CondJmp = 11, Alloc = 12, CondExecPredClean = 13,
        CondExecPredCleanEnd = 14, MarkVsFetchDone = 15,
    };

    struct ControlFlowExecInstruction
    {
        uint32_t address : 12;
        uint32_t count : 3;
        uint32_t isYield : 1;
        uint32_t sequence : 12;
        uint32_t vertexCacheHigh : 4;
        uint32_t vertexCacheLow : 2;
        uint32_t : 7;
        uint32_t isPredicateClean : 1;
        uint32_t : 1;
        uint32_t absoluteAddressing : 1;
        ControlFlowOpcode opcode : 4;
    };

    struct ControlFlowCondExecInstruction
    {
        uint32_t address : 12;
        uint32_t count : 3;
        uint32_t isYield : 1;
        uint32_t sequence : 12;
        uint32_t vertexCacheHigh : 4;
        uint32_t vertexCacheLow : 2;
        uint32_t boolAddress : 8;
        uint32_t condition : 1;
        uint32_t absoluteAddressing : 1;
        ControlFlowOpcode opcode : 4;
    };

    struct ControlFlowCondExecPredInstruction
    {
        uint32_t address : 12;
        uint32_t count : 3;
        uint32_t isYield : 1;
        uint32_t sequence : 12;
        uint32_t vertexCacheHigh : 4;
        uint32_t vertexCacheLow : 2;
        uint32_t : 7;
        uint32_t isPredicateClean : 1;
        uint32_t condition : 1;
        uint32_t absoluteAddressing : 1;
        ControlFlowOpcode opcode : 4;
    };

    struct ControlFlowLoopStartInstruction
    {
        uint32_t address : 13;
        uint32_t isRepeat : 1;
        uint32_t : 2;
        uint32_t loopId : 5;
        uint32_t : 11;
        uint32_t : 11;
        uint32_t absoluteAddressing : 1;
        ControlFlowOpcode opcode : 4;
    };

    struct ControlFlowLoopEndInstruction
    {
        uint32_t address : 13;
        uint32_t : 3;
        uint32_t loopId : 5;
        uint32_t isPredicatedBreak : 1;
        uint32_t : 10;
        uint32_t : 10;
        uint32_t condition : 1;
        uint32_t absoluteAddressing : 1;
        ControlFlowOpcode opcode : 4;
    };

    struct ControlFlowCondJmpInstruction
    {
        uint32_t address : 13;
        uint32_t isUnconditional : 1;
        uint32_t isPredicated : 1;
        uint32_t : 17;
        uint32_t : 1;
        uint32_t direction : 1;
        uint32_t boolAddress : 8;
        uint32_t condition : 1;
        uint32_t absoluteAddressing : 1;
        ControlFlowOpcode opcode : 4;
    };

    union ControlFlowInstruction
    {
        ControlFlowExecInstruction exec;
        ControlFlowCondExecInstruction condExec;
        ControlFlowCondExecPredInstruction condExecPred;
        ControlFlowLoopStartInstruction loopStart;
        ControlFlowLoopEndInstruction loopEnd;
        ControlFlowCondJmpInstruction condJmp;

        struct
        {
            uint32_t : 32;
            uint32_t : 12;
            ControlFlowOpcode opcode : 4;
        };
    };

    enum class FetchOpcode : uint32_t
    {
        VertexFetch = 0, TextureFetch = 1, GetTextureBorderColorFrac = 16, GetTextureComputedLod = 17,
        GetTextureGradients = 18, GetTextureWeights = 19, SetTextureLod = 24, SetTextureGradientsHorz = 25,
        SetTextureGradientsVert = 26
    };

    enum class FetchDestinationSwizzle : uint32_t
    {
        X = 0, Y = 1, Z = 2, W = 3, Zero = 4, One = 5, Keep = 7
    };

    struct VertexFetchInstruction
    {
        struct
        {
            FetchOpcode opcode : 5;
            uint32_t srcRegister : 6;
            uint32_t srcRegisterAm : 1;
            uint32_t dstRegister : 6;
            uint32_t dstRegisterAam : 1;
            uint32_t mustBeOne : 1;
            uint32_t constIndex : 5;
            uint32_t constIndexSelect : 2;
            uint32_t prefetchCount : 3;
            uint32_t srcSwizzle : 2;
        };
        struct
        {
            uint32_t dstSwizzle : 12;
            uint32_t formatCompAll : 1;   // signed
            uint32_t numFormatAll : 1;    // 0 fraction (normalized), 1 integer
            uint32_t signedRfModeAll : 1; // 0 clamp -1, 1 no zero
            uint32_t isIndexRounded : 1;
            uint32_t format : 6;
            uint32_t reserved2 : 2;
            int32_t expAdjust : 6;
            uint32_t isMiniFetch : 1;
            uint32_t isPredicated : 1;
        };
        struct
        {
            uint32_t stride : 8;  // dwords
            int32_t offset : 23;  // dwords
            uint32_t predicateCondition : 1;
        };
    };

    enum class TextureDimension : uint32_t
    {
        Texture1D, Texture2D, Texture3D, TextureCube
    };

    struct TextureFetchInstruction
    {
        struct
        {
            FetchOpcode opcode : 5;
            uint32_t srcRegister : 6;
            uint32_t srcRegisterAm : 1;
            uint32_t dstRegister : 6;
            uint32_t dstRegisterAm : 1;
            uint32_t fetchValidOnly : 1;
            uint32_t constIndex : 5;
            uint32_t texCoordDenorm : 1;
            uint32_t srcSwizzle : 6;
        };
        struct
        {
            uint32_t dstSwizzle : 12;
            uint32_t magFilter : 2;
            uint32_t minFilter : 2;
            uint32_t mipFilter : 2;
            uint32_t anisoFilter : 3;
            uint32_t arbitraryFilter : 3;
            uint32_t volMagFilter : 2;
            uint32_t volMinFilter : 2;
            uint32_t useCompLod : 1;
            uint32_t useRegLod : 1;
            uint32_t : 1;
            uint32_t isPredicated : 1;
        };
        struct
        {
            uint32_t useRegGradients : 1;
            uint32_t sampleLocation : 1;
            int32_t lodBias : 7;
            uint32_t : 5;
            TextureDimension dimension : 2;
            int32_t offsetX : 5;
            int32_t offsetY : 5;
            int32_t offsetZ : 5;
            uint32_t predCondition : 1;
        };
    };

    enum class AluScalarOpcode : uint32_t
    {
        Adds = 0, AddsPrev = 1, Muls = 2, MulsPrev = 3, MulsPrev2 = 4, Maxs = 5, Mins = 6, Seqs = 7, Sgts = 8,
        Sges = 9, Snes = 10, Frcs = 11, Truncs = 12, Floors = 13, Exp = 14, Logc = 15, Log = 16, Rcpc = 17,
        Rcpf = 18, Rcp = 19, Rsqc = 20, Rsqf = 21, Rsq = 22, MaxAs = 23, MaxAsf = 24, Subs = 25, SubsPrev = 26,
        SetpEq = 27, SetpNe = 28, SetpGt = 29, SetpGe = 30, SetpInv = 31, SetpPop = 32, SetpClr = 33,
        SetpRstr = 34, KillsEq = 35, KillsGt = 36, KillsGe = 37, KillsNe = 38, KillsOne = 39, Sqrt = 40,
        Mulsc0 = 42, Mulsc1 = 43, Addsc0 = 44, Addsc1 = 45, Subsc0 = 46, Subsc1 = 47, Sin = 48, Cos = 49,
        RetainPrev = 50
    };

    enum class AluVectorOpcode : uint32_t
    {
        Add = 0, Mul = 1, Max = 2, Min = 3, Seq = 4, Sgt = 5, Sge = 6, Sne = 7, Frc = 8, Trunc = 9, Floor = 10,
        Mad = 11, CndEq = 12, CndGe = 13, CndGt = 14, Dp4 = 15, Dp3 = 16, Dp2Add = 17, Cube = 18, Max4 = 19,
        SetpEqPush = 20, SetpNePush = 21, SetpGtPush = 22, SetpGePush = 23, KillEq = 24, KillGt = 25, KillGe = 26,
        KillNe = 27, Dst = 28, MaxA = 29
    };

    enum class ExportRegister : uint32_t
    {
        VSPosition = 62,
        VSPointSizeEdgeFlagKillVertex = 63,
        PSColor0 = 0, PSColor1, PSColor2, PSColor3,
        PSDepth = 61,
    };

    struct AluInstruction
    {
        struct
        {
            uint32_t vectorDest : 6;
            uint32_t vectorDestRelative : 1;
            uint32_t absConstants : 1;
            uint32_t scalarDest : 6;
            uint32_t scalarDestRelative : 1;
            uint32_t exportData : 1;
            uint32_t vectorWriteMask : 4;
            uint32_t scalarWriteMask : 4;
            uint32_t vectorSaturate : 1;
            uint32_t scalarSaturate : 1;
            AluScalarOpcode scalarOpcode : 6;
        };
        struct
        {
            uint32_t src3Swizzle : 8;
            uint32_t src2Swizzle : 8;
            uint32_t src1Swizzle : 8;
            uint32_t src3Negate : 1;
            uint32_t src2Negate : 1;
            uint32_t src1Negate : 1;
            uint32_t predicateCondition : 1;
            uint32_t isPredicated : 1;
            uint32_t constAddressRegisterRelative : 1;
            uint32_t const1Relative : 1;
            uint32_t const0Relative : 1;
        };
        struct
        {
            uint32_t src3Register : 8;
            uint32_t src2Register : 8;
            uint32_t src1Register : 8;
            AluVectorOpcode vectorOpcode : 5;
            uint32_t src3Select : 1;
            uint32_t src2Select : 1;
            uint32_t src1Select : 1;
        };
    };
}
