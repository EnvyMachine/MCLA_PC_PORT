// See translator.h. The ALU and control-flow translation follows XenosRecomp's
// shader_recompiler.cpp (MIT License, Copyright (c) 2025 hedge-dev and
// contributors).
#include "translator.h"
#include "ucode.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace render::xenos
{
    extern const char kCommonHlsl[];
    const char* CommonHlsl() { return kCommonHlsl; }

    namespace
    {
        constexpr char SWIZZLES[] = { 'x', 'y', 'z', 'w', '0', '1', '_', '_' };

        constexpr const char* USAGE_VARIABLES[] = { "Position", "BlendWeight", "BlendIndices", "Normal", "PointSize",
                                                    "TexCoord", "Tangent", "Binormal", "TessFactor", "PositionT",
                                                    "Color", "Fog", "Depth", "Sample" };

        constexpr const char* USAGE_SEMANTICS[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE",
                                                    "TEXCOORD", "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT",
                                                    "COLOR", "FOG", "DEPTH", "SAMPLE" };

        // Every vertex shader writes, and every pixel shader reads, the same list,
        // so the signatures always link.
        constexpr std::pair<DeclUsage, uint32_t> INTERPOLATORS[] = {
            { DeclUsage::TexCoord, 0 },  { DeclUsage::TexCoord, 1 },  { DeclUsage::TexCoord, 2 },
            { DeclUsage::TexCoord, 3 },  { DeclUsage::TexCoord, 4 },  { DeclUsage::TexCoord, 5 },
            { DeclUsage::TexCoord, 6 },  { DeclUsage::TexCoord, 7 },  { DeclUsage::TexCoord, 8 },
            { DeclUsage::TexCoord, 9 },  { DeclUsage::TexCoord, 10 }, { DeclUsage::TexCoord, 11 },
            { DeclUsage::TexCoord, 12 }, { DeclUsage::TexCoord, 13 }, { DeclUsage::TexCoord, 14 },
            { DeclUsage::TexCoord, 15 }, { DeclUsage::Color, 0 },     { DeclUsage::Color, 1 },
        };

        FetchDestinationSwizzle DestSwizzle(uint32_t dstSwizzle, uint32_t index)
        {
            return FetchDestinationSwizzle((dstSwizzle >> (index * 3)) & 0x7);
        }

        bool AnyComponentWritten(uint32_t dstSwizzle)
        {
            for (uint32_t i = 0; i < 4; i++)
                if (DestSwizzle(dstSwizzle, i) <= FetchDestinationSwizzle::W)
                    return true;
            return false;
        }

        struct Translator
        {
            std::string out;
            uint32_t indentation = 0;
            bool isPixel = false;
            Translation* result = nullptr;
            std::unordered_map<uint32_t, std::string> interpolators; // VS export register -> output
            std::unordered_map<uint32_t, uint32_t> ifEndLabels;
            uint32_t loopCount = 0;
            uint32_t lastVfc = 0, lastStride = 0;
            uint32_t psOutputs = 0; // PixelShaderOutputs declared by the container
            const Linkage* linkage = nullptr; // raw shaders: the partner stage's semantics

            template <class... Args>
            void print(std::format_string<Args...> f, Args&&... args)
            {
                std::format_to(std::back_inserter(out), f, std::forward<Args>(args)...);
            }

            template <class... Args>
            void println(std::format_string<Args...> f, Args&&... args)
            {
                std::format_to(std::back_inserter(out), f, std::forward<Args>(args)...);
                out += '\n';
            }

            void indent()
            {
                for (uint32_t i = 0; i < indentation; i++)
                    out += '\t';
            }

            const char* Constants() const { return isPixel ? "g_PS" : "g_VS"; }

            void printDstSwizzle(uint32_t dstSwizzle, bool operand)
            {
                for (uint32_t i = 0; i < 4; i++)
                {
                    auto swizzle = DestSwizzle(dstSwizzle, i);
                    if (swizzle <= FetchDestinationSwizzle::W)
                        out += SWIZZLES[operand ? uint32_t(swizzle) : i];
                }
            }

            void printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle)
            {
                for (uint32_t i = 0; i < 4; i++)
                {
                    auto swizzle = DestSwizzle(dstSwizzle, i);
                    if (swizzle == FetchDestinationSwizzle::Zero)
                    {
                        indent();
                        println("r{}.{} = 0.0;", dstRegister, SWIZZLES[i]);
                    }
                    else if (swizzle == FetchDestinationSwizzle::One)
                    {
                        indent();
                        println("r{}.{} = 1.0;", dstRegister, SWIZZLES[i]);
                    }
                }
            }

            void BeginPredicated(bool predicated, bool condition)
            {
                if (!predicated)
                    return;
                indent();
                println("if ({}p0)", condition ? "" : "!");
                indent();
                out += "{\n";
                ++indentation;
            }

            void EndPredicated(bool predicated)
            {
                if (!predicated)
                    return;
                --indentation;
                indent();
                out += "}\n";
            }

            void Vfetch(const VertexFetchInstruction& instr)
            {
                BeginPredicated(instr.isPredicated, instr.predicateCondition);
                if (!instr.isMiniFetch)
                {
                    lastVfc = instr.constIndex * 3 + instr.constIndexSelect;
                    lastStride = instr.stride;
                    result->vertexFetchSlots[lastVfc / 32] |= 1u << (lastVfc % 32);
                    indent();
                    println("vIndex = uint(floor(r{}.{}{}));", uint32_t(instr.srcRegister), SWIZZLES[instr.srcSwizzle & 3],
                            instr.isIndexRounded ? " + 0.5" : "");
                }
                if (AnyComponentWritten(instr.dstSwizzle))
                {
                    indent();
                    print("r{}.", uint32_t(instr.dstRegister));
                    printDstSwizzle(instr.dstSwizzle, false);
                    print(" = VFetch({}, vIndex, {}, {}, {}, {}, {}, {}, {}).", lastVfc, lastStride, int32_t(instr.offset),
                          uint32_t(instr.format), instr.formatCompAll ? "true" : "false",
                          instr.numFormatAll ? "true" : "false", instr.signedRfModeAll ? "true" : "false",
                          int32_t(instr.expAdjust));
                    printDstSwizzle(instr.dstSwizzle, true);
                    out += ";\n";
                }
                printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);
                EndPredicated(instr.isPredicated);
            }

            void Tfetch(const TextureFetchInstruction& instr)
            {
                std::string src = std::format("r{}.", uint32_t(instr.srcRegister));
                for (uint32_t i = 0; i < 3; i++)
                    src += SWIZZLES[(instr.srcSwizzle >> (i * 2)) & 0x3];
                auto srcComponents = [&](uint32_t n) { return src.substr(0, src.find('.') + 1 + n); };

                switch (instr.opcode)
                {
                case FetchOpcode::SetTextureLod:
                    BeginPredicated(instr.isPredicated, instr.predCondition);
                    indent();
                    println("gradHLod.w = {};", srcComponents(1));
                    EndPredicated(instr.isPredicated);
                    return;
                case FetchOpcode::SetTextureGradientsHorz:
                    BeginPredicated(instr.isPredicated, instr.predCondition);
                    indent();
                    println("gradHLod.xyz = {};", srcComponents(3));
                    EndPredicated(instr.isPredicated);
                    return;
                case FetchOpcode::SetTextureGradientsVert:
                    BeginPredicated(instr.isPredicated, instr.predCondition);
                    indent();
                    println("gradV.xyz = {};", srcComponents(3));
                    EndPredicated(instr.isPredicated);
                    return;
                default:
                    break;
                }

                if (!AnyComponentWritten(instr.dstSwizzle))
                {
                    BeginPredicated(instr.isPredicated, instr.predCondition);
                    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);
                    EndPredicated(instr.isPredicated);
                    return;
                }

                uint32_t slot = instr.constIndex;
                std::string value;
                bool unnormalized = instr.texCoordDenorm;
                float lodBias = float(int32_t(instr.lodBias)) / 16.0f;
                // 0 implicit, 1 explicit LOD, 2 explicit gradients.
                uint32_t mode = instr.useRegGradients ? 2 : (instr.useRegLod || !isPixel) ? 1 : 0;
                std::string lod = instr.useRegLod ? std::format("gradHLod.w + {}", lodBias) : std::format("{}", lodBias);
                float offX = instr.offsetX * 0.5f, offY = instr.offsetY * 0.5f, offZ = instr.offsetZ * 0.5f;

                switch (instr.opcode)
                {
                case FetchOpcode::TextureFetch:
                    result->textureSlots |= 1u << slot;
                    result->textureDims[slot] = uint8_t(instr.dimension);
                    switch (instr.dimension)
                    {
                    case TextureDimension::Texture1D:
                        value = std::format("TFetch2D({}, float2({}, 0.0), {}, float2({}, 0.0), {}, {}, gradHLod.xy, gradV.xy, {})",
                                            slot, srcComponents(1), unnormalized, offX, mode, lod, lodBias);
                        break;
                    case TextureDimension::Texture2D:
                        value = std::format("TFetch2D({}, {}, {}, float2({}, {}), {}, {}, gradHLod.xy, gradV.xy, {})", slot,
                                            srcComponents(2), unnormalized, offX, offY, mode, lod, lodBias);
                        break;
                    case TextureDimension::Texture3D:
                        value = std::format("TFetch3D({}, {}, {}, float3({}, {}, {}), {}, {}, gradHLod.xyz, gradV.xyz, {})",
                                            slot, srcComponents(3), unnormalized, offX, offY, offZ, mode, lod, lodBias);
                        break;
                    case TextureDimension::TextureCube:
                        value = std::format("TFetchCube({}, {}, cubeMapData, {}, {}, {})", slot, srcComponents(3),
                                            mode == 2 ? 1 : mode, lod, lodBias);
                        break;
                    }
                    break;
                case FetchOpcode::GetTextureGradients:
                {
                    std::string s2 = srcComponents(2);
                    value = isPixel ? std::format("float4(ddx({0}.x), ddy({0}.x), ddx({0}.y), ddy({0}.y))", "(" + s2 + ")")
                                    : std::string("float4(0.0, 0.0, 0.0, 0.0)");
                    break;
                }
                case FetchOpcode::GetTextureComputedLod:
                    result->textureSlots |= 1u << slot;
                    result->textureDims[slot] = uint8_t(TextureDimension::Texture2D);
                    value = isPixel ? std::format("GetLod2D({}, {}, {}).xxxx", slot, srcComponents(2), unnormalized)
                                    : std::string("float4(0.0, 0.0, 0.0, 0.0)");
                    break;
                case FetchOpcode::GetTextureWeights:
                    result->textureSlots |= 1u << slot;
                    result->textureDims[slot] = uint8_t(TextureDimension::Texture2D);
                    value = std::format("GetWeights2D({}, {}, {}, float2({}, {}))", slot, srcComponents(2), unnormalized,
                                        offX, offY);
                    break;
                default: // getBCF and unknown: zero
                    value = "float4(0.0, 0.0, 0.0, 0.0)";
                    break;
                }

                BeginPredicated(instr.isPredicated, instr.predCondition);
                indent();
                print("r{}.", uint32_t(instr.dstRegister));
                printDstSwizzle(instr.dstSwizzle, false);
                print(" = ({}).", value);
                printDstSwizzle(instr.dstSwizzle, true);
                out += ";\n";
                printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);
                EndPredicated(instr.isPredicated);
            }

            void Alu(const AluInstruction& instr);
            bool Run(const uint8_t* container, size_t containerSize, const uint8_t* program, uint32_t programDwords);
        };

        void Translator::Alu(const AluInstruction& instr)
        {
            BeginPredicated(instr.isPredicated, instr.predicateCondition);

            enum
            {
                VECTOR_0,
                VECTOR_1,
                VECTOR_2,
                SCALAR_0,
                SCALAR_1,
                SCALAR_CONSTANT_0,
                SCALAR_CONSTANT_1
            };

            auto op = [&](size_t operand) {
                uint32_t reg = 0, swizzle = 0, constIndex = 0;
                bool select = true, negate = false, abs = false;

                switch (operand)
                {
                case SCALAR_CONSTANT_0:
                    reg = instr.src3Register;
                    swizzle = instr.src3Swizzle;
                    select = false;
                    negate = instr.src3Negate;
                    abs = instr.absConstants;
                    break;
                case SCALAR_CONSTANT_1:
                    reg = (uint32_t(instr.scalarOpcode) & 1) | (instr.src3Select << 1) | (instr.src3Swizzle & 0x3C);
                    swizzle = instr.src3Swizzle;
                    select = true;
                    negate = instr.src3Negate;
                    abs = instr.absConstants;
                    break;
                default:
                    switch (operand)
                    {
                    case VECTOR_0:
                        reg = instr.src1Register;
                        swizzle = instr.src1Swizzle;
                        select = instr.src1Select;
                        negate = instr.src1Negate;
                        break;
                    case VECTOR_1:
                        reg = instr.src2Register;
                        swizzle = instr.src2Swizzle;
                        select = instr.src2Select;
                        negate = instr.src2Negate;
                        constIndex = instr.src1Select ? 0 : 1;
                        break;
                    case VECTOR_2:
                    case SCALAR_0:
                    case SCALAR_1:
                        reg = instr.src3Register;
                        swizzle = instr.src3Swizzle;
                        select = instr.src3Select;
                        negate = instr.src3Negate;
                        constIndex = (instr.src1Select ? 0 : 1) + (instr.src2Select ? 0 : 1);
                        break;
                    }
                    if (select)
                    {
                        abs = (reg & 0x80) != 0;
                        reg &= 0x3F;
                    }
                    else
                    {
                        abs = instr.absConstants;
                    }
                    break;
                }

                std::string regFormatted;
                if (select)
                {
                    regFormatted = std::format("r{}", reg);
                }
                else
                {
                    bool relative = constIndex == 0 ? instr.const0Relative : instr.const1Relative;
                    if (relative)
                        regFormatted = std::format("{}[({} + {}) & 255]", Constants(), reg,
                                                   instr.constAddressRegisterRelative ? "a0" : "aL");
                    else
                        regFormatted = std::format("{}[{}]", Constants(), reg);
                }

                std::string r;
                if (negate)
                    r += '-';
                if (abs)
                    r += "abs(";
                r += regFormatted;
                r += '.';

                switch (operand)
                {
                case VECTOR_0:
                case VECTOR_1:
                case VECTOR_2:
                {
                    uint32_t mask;
                    switch (instr.vectorOpcode)
                    {
                    case AluVectorOpcode::Dp2Add:
                        mask = (operand == VECTOR_2) ? 0b1 : 0b11;
                        break;
                    case AluVectorOpcode::Dp3:
                        mask = 0b111;
                        break;
                    case AluVectorOpcode::Dp4:
                    case AluVectorOpcode::Max4:
                        mask = 0b1111;
                        break;
                    default:
                        mask = instr.vectorWriteMask != 0 ? instr.vectorWriteMask : 0b1;
                        break;
                    }
                    for (uint32_t i = 0; i < 4; i++)
                        if ((mask >> i) & 0x1)
                            r += SWIZZLES[((swizzle >> (i * 2)) + i) & 0x3];
                    break;
                }
                case SCALAR_0:
                case SCALAR_CONSTANT_0:
                    r += SWIZZLES[((swizzle >> 6) + 3) & 0x3];
                    break;
                case SCALAR_1:
                case SCALAR_CONSTANT_1:
                    r += SWIZZLES[swizzle & 0x3];
                    break;
                }

                if (abs)
                    r += ")";
                return r;
            };

            switch (instr.vectorOpcode)
            {
            case AluVectorOpcode::KillEq:
                indent();
                println("clip(any({} == {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
                break;
            case AluVectorOpcode::KillGt:
                indent();
                println("clip(any({} > {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
                break;
            case AluVectorOpcode::KillGe:
                indent();
                println("clip(any({} >= {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
                break;
            case AluVectorOpcode::KillNe:
                indent();
                println("clip(any({} != {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
                break;
            default:
                break;
            }

            std::string exportRegister;
            if (instr.exportData)
            {
                if (isPixel)
                {
                    switch (instr.vectorDest)
                    {
                    case 0: case 1: case 2: case 3:
                        if (psOutputs & (PIXEL_SHADER_OUTPUT_COLOR0 << instr.vectorDest))
                        {
                            exportRegister = std::format("oC{}", uint32_t(instr.vectorDest));
                            result->colorOutputs |= 1u << instr.vectorDest;
                        }
                        else
                        {
                            exportRegister = "oUnused";
                        }
                        break;
                    case uint32_t(ExportRegister::PSDepth):
                        exportRegister = "oDepth4";
                        result->writesDepth = (psOutputs & PIXEL_SHADER_OUTPUT_DEPTH) != 0;
                        break;
                    default:
                        exportRegister = "oUnused"; // memory export
                        break;
                    }
                }
                else
                {
                    if (instr.vectorDest == uint32_t(ExportRegister::VSPosition))
                        exportRegister = "oPos";
                    else if (auto it = interpolators.find(instr.vectorDest); it != interpolators.end())
                        exportRegister = it->second;
                    else
                        exportRegister = "oUnused"; // point size / memory export / unmapped interpolator
                }
            }

            if (instr.vectorOpcode >= AluVectorOpcode::SetpEqPush && instr.vectorOpcode <= AluVectorOpcode::SetpGePush)
            {
                indent();
                print("p0 = {} == 0.0 && {} ", op(VECTOR_0), op(VECTOR_1));
                switch (instr.vectorOpcode)
                {
                case AluVectorOpcode::SetpEqPush: out += "=="; break;
                case AluVectorOpcode::SetpNePush: out += "!="; break;
                case AluVectorOpcode::SetpGtPush: out += ">"; break;
                case AluVectorOpcode::SetpGePush: out += ">="; break;
                default: break;
                }
                out += " 0.0;\n";
            }
            else if (instr.vectorOpcode >= AluVectorOpcode::MaxA)
            {
                indent();
                println("a0 = (int)clamp(floor(({}).w + 0.5), -256.0, 255.0);", op(VECTOR_0));
            }

            uint32_t vectorWriteMask = instr.vectorWriteMask;
            if (instr.exportData)
                vectorWriteMask &= ~instr.scalarWriteMask;

            if (vectorWriteMask != 0)
            {
                indent();
                if (!exportRegister.empty())
                    out += exportRegister + '.';
                else
                    print("r{}.", uint32_t(instr.vectorDest));
                for (uint32_t i = 0; i < 4; i++)
                    if ((vectorWriteMask >> i) & 0x1)
                        out += SWIZZLES[i];
                out += " = ";
                if (instr.vectorSaturate)
                    out += "saturate(";

                switch (instr.vectorOpcode)
                {
                case AluVectorOpcode::Add: print("{} + {}", op(VECTOR_0), op(VECTOR_1)); break;
                case AluVectorOpcode::Mul: print("{} * {}", op(VECTOR_0), op(VECTOR_1)); break;
                case AluVectorOpcode::Max:
                case AluVectorOpcode::MaxA: print("max({}, {})", op(VECTOR_0), op(VECTOR_1)); break;
                case AluVectorOpcode::Min: print("min({}, {})", op(VECTOR_0), op(VECTOR_1)); break;
                case AluVectorOpcode::Seq: print("{} == {}", op(VECTOR_0), op(VECTOR_1)); break;
                case AluVectorOpcode::Sgt: print("{} > {}", op(VECTOR_0), op(VECTOR_1)); break;
                case AluVectorOpcode::Sge: print("{} >= {}", op(VECTOR_0), op(VECTOR_1)); break;
                case AluVectorOpcode::Sne: print("{} != {}", op(VECTOR_0), op(VECTOR_1)); break;
                case AluVectorOpcode::Frc: print("frac({})", op(VECTOR_0)); break;
                case AluVectorOpcode::Trunc: print("trunc({})", op(VECTOR_0)); break;
                case AluVectorOpcode::Floor: print("floor({})", op(VECTOR_0)); break;
                case AluVectorOpcode::Mad: print("{} * {} + {}", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2)); break;
                case AluVectorOpcode::CndEq:
                    print("select({} == 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
                    break;
                case AluVectorOpcode::CndGe:
                    print("select({} >= 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
                    break;
                case AluVectorOpcode::CndGt:
                    print("select({} > 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
                    break;
                case AluVectorOpcode::Dp4:
                case AluVectorOpcode::Dp3: print("dot({}, {})", op(VECTOR_0), op(VECTOR_1)); break;
                case AluVectorOpcode::Dp2Add: print("dot({}, {}) + {}", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2)); break;
                case AluVectorOpcode::Cube: print("cube(r{}, cubeMapData)", uint32_t(instr.src1Register)); break;
                case AluVectorOpcode::Max4: print("max4({})", op(VECTOR_0)); break;
                case AluVectorOpcode::SetpEqPush:
                case AluVectorOpcode::SetpNePush:
                case AluVectorOpcode::SetpGtPush:
                case AluVectorOpcode::SetpGePush: print("p0 ? 0.0 : {} + 1.0", op(VECTOR_0)); break;
                case AluVectorOpcode::KillEq: print("any({} == {})", op(VECTOR_0), op(VECTOR_1)); break;
                case AluVectorOpcode::KillGt: print("any({} > {})", op(VECTOR_0), op(VECTOR_1)); break;
                case AluVectorOpcode::KillGe: print("any({} >= {})", op(VECTOR_0), op(VECTOR_1)); break;
                case AluVectorOpcode::KillNe: print("any({} != {})", op(VECTOR_0), op(VECTOR_1)); break;
                case AluVectorOpcode::Dst: print("dst({}, {})", op(VECTOR_0), op(VECTOR_1)); break;
                default: print("{}", op(VECTOR_0)); break;
                }

                if (instr.vectorSaturate)
                    out += ')';
                out += ";\n";
            }

            if (instr.scalarOpcode != AluScalarOpcode::RetainPrev)
            {
                if (instr.scalarOpcode >= AluScalarOpcode::SetpEq && instr.scalarOpcode <= AluScalarOpcode::SetpRstr)
                {
                    indent();
                    out += "p0 = ";
                    switch (instr.scalarOpcode)
                    {
                    case AluScalarOpcode::SetpEq: print("{} == 0.0", op(SCALAR_0)); break;
                    case AluScalarOpcode::SetpNe: print("{} != 0.0", op(SCALAR_0)); break;
                    case AluScalarOpcode::SetpGt: print("{} > 0.0", op(SCALAR_0)); break;
                    case AluScalarOpcode::SetpGe: print("{} >= 0.0", op(SCALAR_0)); break;
                    case AluScalarOpcode::SetpInv: print("{} == 1.0", op(SCALAR_0)); break;
                    case AluScalarOpcode::SetpPop: print("{} - 1.0 <= 0.0", op(SCALAR_0)); break;
                    case AluScalarOpcode::SetpClr: out += "false"; break;
                    case AluScalarOpcode::SetpRstr: print("{} == 0.0", op(SCALAR_0)); break;
                    default: break;
                    }
                    out += ";\n";
                }

                indent();
                out += "ps = ";
                if (instr.scalarSaturate)
                    out += "saturate(";

                switch (instr.scalarOpcode)
                {
                case AluScalarOpcode::Adds: print("{} + {}", op(SCALAR_0), op(SCALAR_1)); break;
                case AluScalarOpcode::AddsPrev: print("{} + ps", op(SCALAR_0)); break;
                case AluScalarOpcode::Muls: print("{} * {}", op(SCALAR_0), op(SCALAR_1)); break;
                case AluScalarOpcode::MulsPrev:
                case AluScalarOpcode::MulsPrev2: print("{} * ps", op(SCALAR_0)); break;
                case AluScalarOpcode::Maxs:
                case AluScalarOpcode::MaxAs:
                case AluScalarOpcode::MaxAsf: print("max({}, {})", op(SCALAR_0), op(SCALAR_1)); break;
                case AluScalarOpcode::Mins: print("min({}, {})", op(SCALAR_0), op(SCALAR_1)); break;
                case AluScalarOpcode::Seqs: print("{} == 0.0", op(SCALAR_0)); break;
                case AluScalarOpcode::Sgts: print("{} > 0.0", op(SCALAR_0)); break;
                case AluScalarOpcode::Sges: print("{} >= 0.0", op(SCALAR_0)); break;
                case AluScalarOpcode::Snes: print("{} != 0.0", op(SCALAR_0)); break;
                case AluScalarOpcode::Frcs: print("frac({})", op(SCALAR_0)); break;
                case AluScalarOpcode::Truncs: print("trunc({})", op(SCALAR_0)); break;
                case AluScalarOpcode::Floors: print("floor({})", op(SCALAR_0)); break;
                case AluScalarOpcode::Exp: print("exp2({})", op(SCALAR_0)); break;
                case AluScalarOpcode::Logc:
                case AluScalarOpcode::Log: print("clamp(log2({}), FLT_MIN, FLT_MAX)", op(SCALAR_0)); break;
                case AluScalarOpcode::Rcpc:
                case AluScalarOpcode::Rcpf:
                case AluScalarOpcode::Rcp: print("clamp(rcp({}), FLT_MIN, FLT_MAX)", op(SCALAR_0)); break;
                case AluScalarOpcode::Rsqc:
                case AluScalarOpcode::Rsqf:
                case AluScalarOpcode::Rsq: print("clamp(rsqrt({}), FLT_MIN, FLT_MAX)", op(SCALAR_0)); break;
                case AluScalarOpcode::Subs: print("{} - {}", op(SCALAR_0), op(SCALAR_1)); break;
                case AluScalarOpcode::SubsPrev: print("{} - ps", op(SCALAR_0)); break;
                case AluScalarOpcode::SetpEq:
                case AluScalarOpcode::SetpNe:
                case AluScalarOpcode::SetpGt:
                case AluScalarOpcode::SetpGe: out += "p0 ? 0.0 : 1.0"; break;
                case AluScalarOpcode::SetpInv: print("{0} == 0.0 ? 1.0 : {0}", op(SCALAR_0)); break;
                case AluScalarOpcode::SetpPop: print("p0 ? 0.0 : ({} - 1.0)", op(SCALAR_0)); break;
                case AluScalarOpcode::SetpClr: out += "FLT_MAX"; break;
                case AluScalarOpcode::SetpRstr: print("p0 ? 0.0 : {}", op(SCALAR_0)); break;
                case AluScalarOpcode::KillsEq: print("{} == 0.0", op(SCALAR_0)); break;
                case AluScalarOpcode::KillsGt: print("{} > 0.0", op(SCALAR_0)); break;
                case AluScalarOpcode::KillsGe: print("{} >= 0.0", op(SCALAR_0)); break;
                case AluScalarOpcode::KillsNe: print("{} != 0.0", op(SCALAR_0)); break;
                case AluScalarOpcode::KillsOne: print("{} == 1.0", op(SCALAR_0)); break;
                case AluScalarOpcode::Sqrt: print("sqrt({})", op(SCALAR_0)); break;
                case AluScalarOpcode::Mulsc0:
                case AluScalarOpcode::Mulsc1: print("{} * {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1)); break;
                case AluScalarOpcode::Addsc0:
                case AluScalarOpcode::Addsc1: print("{} + {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1)); break;
                case AluScalarOpcode::Subsc0:
                case AluScalarOpcode::Subsc1: print("{} - {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1)); break;
                case AluScalarOpcode::Sin: print("sin({})", op(SCALAR_0)); break;
                case AluScalarOpcode::Cos: print("cos({})", op(SCALAR_0)); break;
                default: out += "ps"; break;
                }

                if (instr.scalarSaturate)
                    out += ')';
                out += ";\n";

                if (instr.scalarOpcode == AluScalarOpcode::MaxAs)
                {
                    indent();
                    println("a0 = (int)clamp(floor({} + 0.5), -256.0, 255.0);", op(SCALAR_0));
                }
                else if (instr.scalarOpcode == AluScalarOpcode::MaxAsf)
                {
                    indent();
                    println("a0 = (int)clamp(floor({}), -256.0, 255.0);", op(SCALAR_0));
                }
            }

            uint32_t scalarWriteMask = instr.scalarWriteMask;
            if (instr.exportData)
                scalarWriteMask &= ~instr.vectorWriteMask;

            if (scalarWriteMask != 0)
            {
                indent();
                if (!exportRegister.empty())
                    out += exportRegister + '.';
                else
                    print("r{}.", uint32_t(instr.scalarDest));
                for (uint32_t i = 0; i < 4; i++)
                    if ((scalarWriteMask >> i) & 0x1)
                        out += SWIZZLES[i];
                out += " = ps;\n";
            }

            if (instr.exportData)
            {
                uint32_t zeroMask = instr.scalarDestRelative ? (0b1111 & ~(instr.vectorWriteMask | instr.scalarWriteMask)) : 0;
                uint32_t oneMask = instr.vectorWriteMask & instr.scalarWriteMask;
                for (uint32_t i = 0; i < 4; i++)
                {
                    uint32_t mask = 1u << i;
                    if (zeroMask & mask)
                    {
                        indent();
                        println("{}.{} = 0.0;", exportRegister, SWIZZLES[i]);
                    }
                    else if (oneMask & mask)
                    {
                        indent();
                        println("{}.{} = 1.0;", exportRegister, SWIZZLES[i]);
                    }
                }
            }

            if (instr.scalarOpcode >= AluScalarOpcode::KillsEq && instr.scalarOpcode <= AluScalarOpcode::KillsOne)
            {
                indent();
                out += "clip(ps != 0.0 ? -1 : 1);\n";
            }

            EndPredicated(instr.isPredicated);
        }

        bool Translator::Run(const uint8_t* data, size_t dataSize, const uint8_t* program, uint32_t programDwords)
        {
            // data == nullptr: raw microcode (isPixel and psOutputs set by the caller).
            const Shader* shader = nullptr;
            if (data)
            {
                const auto* container = reinterpret_cast<const ShaderContainer*>(data);
                if ((container->flags & 0xFFFFFF00) != 0x102A1100 || container->shaderOffset >= dataSize)
                {
                    result->error = "not a shader container";
                    return false;
                }
                isPixel = (container->flags & 0x1) == 0;
                shader = reinterpret_cast<const Shader*>(data + container->shaderOffset);
            }
            result->isPixel = isPixel;

            out += CommonHlsl();
            out += "\n";

            // ---- Signature ----
            if (isPixel)
            {
                out += "void main(\n\tin float4 iPos : SV_Position,\n";
                for (auto& [usage, usageIndex] : INTERPOLATORS)
                    println("\tin float4 i{0}{1} : {2}{1},", USAGE_VARIABLES[uint32_t(usage)], usageIndex,
                            USAGE_SEMANTICS[uint32_t(usage)]);
                out += "\tin uint iFace : SV_IsFrontFace";
                if (shader)
                    psOutputs = reinterpret_cast<const PixelShader*>(shader)->outputs;
                for (uint32_t i = 0; i < 4; i++)
                    if (psOutputs & (PIXEL_SHADER_OUTPUT_COLOR0 << i))
                        print(",\n\tout float4 oC{0} : SV_Target{0}", i);
                if (psOutputs & PIXEL_SHADER_OUTPUT_DEPTH)
                    out += ",\n\tout float oDepth : SV_Depth";
                out += ")\n{\n";
                for (uint32_t i = 0; i < 4; i++)
                    if (psOutputs & (PIXEL_SHADER_OUTPUT_COLOR0 << i))
                        println("\toC{} = 0.0;", i);
                if (psOutputs & PIXEL_SHADER_OUTPUT_DEPTH)
                    out += "\toDepth = iPos.z;\n";
                out += "\tfloat4 oDepth4 = 0.0;\n";
            }
            else
            {
                out += "void main(\n\tin uint iVertexId : SV_VertexID,\n\tout float4 oPos : SV_Position";
                for (auto& [usage, usageIndex] : INTERPOLATORS)
                    print(",\n\tout float4 o{0}{1} : {2}{1}", USAGE_VARIABLES[uint32_t(usage)], usageIndex,
                          USAGE_SEMANTICS[uint32_t(usage)]);
                out += ")\n{\n\toPos = 0.0;\n";
                for (auto& [usage, usageIndex] : INTERPOLATORS)
                    println("\to{}{} = 0.0;", USAGE_VARIABLES[uint32_t(usage)], usageIndex);
            }
            out += "\tfloat4 oUnused = 0.0;\n";

            // ---- Registers ----
            bool printedRegisters[32]{};
            if (!shader)
            {
                // The hardware links VS export N to PS register N. Without a container
                // that is TEXCOORDN, unless the partner stage's semantics are known.
                for (uint32_t i = 0; i < 16; i++)
                {
                    std::string name = std::format("TexCoord{}", i);
                    if (linkage && linkage->valid[i])
                        name = std::format("{}{}", USAGE_VARIABLES[linkage->usage[i] & 0xF], uint32_t(linkage->usageIndex[i]));
                    if (isPixel)
                    {
                        println("\tfloat4 r{} = i{};", i, name);
                        printedRegisters[i] = true;
                    }
                    else
                    {
                        interpolators.emplace(i, "o" + name);
                    }
                }
            }
            uint32_t interpolatorCount = shader ? (shader->interpolatorInfo >> 5) & 0x1F : 0;
            if (shader && !isPixel && linkage)
            {
                // The loaded program is D3D's copy linked to the current pixel shader:
                // export N feeds pixel shader register N, so it takes that register's
                // semantic. Exports the pixel shader doesn't read go nowhere.
                for (uint32_t i = 0; i < 16; i++)
                    if (linkage->valid[i])
                        interpolators.emplace(i, std::format("o{}{}", USAGE_VARIABLES[linkage->usage[i] & 0xF],
                                                             uint32_t(linkage->usageIndex[i])));
                interpolatorCount = 0;
            }
            for (uint32_t i = 0; i < interpolatorCount; i++)
            {
                union
                {
                    Interpolator interpolator;
                    uint32_t value;
                };
                if (isPixel)
                {
                    value = reinterpret_cast<const PixelShader*>(shader)->interpolators[i];
                    if (interpolator.reg < 32 && !printedRegisters[interpolator.reg])
                    {
                        println("\tfloat4 r{} = i{}{};", uint32_t(interpolator.reg), USAGE_VARIABLES[uint32_t(interpolator.usage)],
                                uint32_t(interpolator.usageIndex));
                        printedRegisters[interpolator.reg] = true;
                    }
                }
                else
                {
                    auto* vertexShader = reinterpret_cast<const VertexShader*>(shader);
                    value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + vertexShader->vertexElementCount + i];
                    interpolators.emplace(i, std::format("o{}{}", USAGE_VARIABLES[uint32_t(interpolator.usage)],
                                                         uint32_t(interpolator.usageIndex)));
                }
            }
            uint32_t positionRegister = shader ? (shader->fieldC >> 8) & 0xFF : 0xFF;
            for (uint32_t i = 0; i < 32; i++)
            {
                if (printedRegisters[i])
                    continue;
                print("\tfloat4 r{} = ", i);
                if (isPixel && i == positionRegister) // guest pixel index (render targets are sample grids)
                    out += "float4(floor(iPos.xy / g_PixelScale.xy) * float2(iFace ? 1.0 : -1.0, 1.0), 0.0, 0.0);\n";
                else if (!isPixel && i == 0)
                    out += "float4(VertexIndex(iVertexId), 0.0, 0.0, 0.0);\n";
                else
                    out += "0.0;\n";
            }
            out += "\tint a0 = 0;\n\tint aL = 0;\n\tbool p0 = false;\n\tfloat ps = 0.0;\n\tuint vIndex = 0;\n";
            out += "\tfloat4 gradHLod = 0.0;\n\tfloat4 gradV = 0.0;\n\tCubeMapData cubeMapData = (CubeMapData)0;\n";

            // ---- Control flow ----
            const be<uint32_t>* code = reinterpret_cast<const be<uint32_t>*>(program);
            union
            {
                ControlFlowInstruction controlFlow[2];
                struct
                {
                    uint32_t code0, code1, code2, code3;
                };
            };

            uint32_t instrSize = programDwords * 4;
            bool simpleControlFlow = true;
            for (uint32_t at = 0; at + 12 <= instrSize; at += 12)
            {
                const be<uint32_t>* cf = code + at / 4;
                code0 = cf[0];
                code1 = cf[1] & 0xFFFF;
                code2 = (cf[1] >> 16) | (cf[2] << 16);
                code3 = cf[2] >> 16;
                for (auto& cfInstr : controlFlow)
                {
                    uint32_t address = 0;
                    switch (cfInstr.opcode)
                    {
                    case ControlFlowOpcode::Exec:
                    case ControlFlowOpcode::ExecEnd:
                    case ControlFlowOpcode::CondExec:
                    case ControlFlowOpcode::CondExecEnd:
                    case ControlFlowOpcode::CondExecPred:
                    case ControlFlowOpcode::CondExecPredEnd:
                    case ControlFlowOpcode::CondExecPredClean:
                    case ControlFlowOpcode::CondExecPredCleanEnd:
                        address = cfInstr.exec.address;
                        break;
                    case ControlFlowOpcode::CondJmp:
                        if (cfInstr.condJmp.isUnconditional || cfInstr.condJmp.direction)
                            simpleControlFlow = false;
                        else
                            ++ifEndLabels[cfInstr.condJmp.address];
                        break;
                    case ControlFlowOpcode::LoopStart:
                    case ControlFlowOpcode::LoopEnd:
                        // Loops are emitted as structured for-loops only in simple mode.
                        break;
                    case ControlFlowOpcode::CondCall:
                    case ControlFlowOpcode::Return:
                        simpleControlFlow = false;
                        break;
                    default:
                        break;
                    }
                    if (address != 0)
                        instrSize = std::min<uint32_t>(instrSize, address * 12);
                }
            }

            if (simpleControlFlow)
            {
                out += '\n';
                indentation = 1;
            }
            else
            {
                out += "\n\tuint pc = 0;\n\tuint loopIteration = 0;\n\t[loop] while (true)\n\t{\n\t\tswitch (pc)\n\t\t{\n";
            }

            auto emitEpilogue = [&] {
                if (isPixel)
                {
                    if (result->colorOutputs & 1)
                    {
                        indent();
                        out += "AlphaTest(oC0.w);\n";
                    }
                    for (uint32_t i = 0; i < 4; i++)
                    {
                        if (result->colorOutputs & (1u << i))
                        {
                            indent();
                            out += "oC" + std::to_string(i) + " = ColorOutput(oC" + std::to_string(i) + ", " + std::to_string(i) +
                                   ");\n";
                        }
                    }
                    if (result->writesDepth)
                    {
                        indent();
                        out += "oDepth = oDepth4.x;\n";
                    }
                }
                else
                {
                    indent();
                    out += "oPos = GuestToHostPosition(oPos);\n";
                }
            };

            uint32_t pc = 0;
            std::vector<uint32_t> openLoops; // loop numbers, simple mode
            for (uint32_t at = 0; at + 12 <= instrSize; at += 12)
            {
                const be<uint32_t>* cf = code + at / 4;
                code0 = cf[0];
                code1 = cf[1] & 0xFFFF;
                code2 = (cf[1] >> 16) | (cf[2] << 16);
                code3 = cf[2] >> 16;

                for (auto& cfInstr : controlFlow)
                {
                    if (!simpleControlFlow)
                    {
                        indentation = 3;
                        println("\t\tcase {}:", pc);
                    }
                    else if (auto it = ifEndLabels.find(pc); it != ifEndLabels.end())
                    {
                        for (uint32_t i = 0; i < it->second; i++)
                        {
                            --indentation;
                            indent();
                            out += "}\n";
                        }
                    }
                    ++pc;

                    uint32_t address = 0, count = 0, sequence = 0;
                    bool shouldReturn = false;
                    std::string clauseCondition;

                    switch (cfInstr.opcode)
                    {
                    case ControlFlowOpcode::Exec:
                    case ControlFlowOpcode::ExecEnd:
                        address = cfInstr.exec.address;
                        count = cfInstr.exec.count;
                        sequence = cfInstr.exec.sequence;
                        shouldReturn = cfInstr.opcode == ControlFlowOpcode::ExecEnd;
                        break;
                    case ControlFlowOpcode::CondExec:
                    case ControlFlowOpcode::CondExecEnd:
                        address = cfInstr.condExec.address;
                        count = cfInstr.condExec.count;
                        sequence = cfInstr.condExec.sequence;
                        shouldReturn = cfInstr.opcode == ControlFlowOpcode::CondExecEnd;
                        clauseCondition = std::format("BoolConstant({}) == {}", uint32_t(cfInstr.condExec.boolAddress),
                                                      cfInstr.condExec.condition ? "true" : "false");
                        break;
                    case ControlFlowOpcode::CondExecPred:
                    case ControlFlowOpcode::CondExecPredEnd:
                    case ControlFlowOpcode::CondExecPredClean:
                    case ControlFlowOpcode::CondExecPredCleanEnd:
                        address = cfInstr.condExecPred.address;
                        count = cfInstr.condExecPred.count;
                        sequence = cfInstr.condExecPred.sequence;
                        shouldReturn = cfInstr.opcode == ControlFlowOpcode::CondExecPredEnd ||
                                       cfInstr.opcode == ControlFlowOpcode::CondExecPredCleanEnd;
                        clauseCondition = std::format("p0 == {}", cfInstr.condExecPred.condition ? "true" : "false");
                        break;
                    case ControlFlowOpcode::LoopStart:
                    {
                        uint32_t id = cfInstr.loopStart.loopId;
                        if (simpleControlFlow)
                        {
                            uint32_t n = loopCount++;
                            openLoops.push_back(n);
                            indent();
                            println("uint loopConstant{0} = LoopConstant({1});", n, id);
                            indent();
                            println("aL = int((loopConstant{} >> 8) & 0xFF);", n);
                            indent();
                            println("[loop] for (uint loopIt{0} = 0; loopIt{0} < (loopConstant{0} & 0xFF); loopIt{0}++)", n);
                            indent();
                            out += "{\n";
                            ++indentation;
                        }
                        else
                        {
                            println("\t\t\tloopIteration = 0;");
                            println("\t\t\taL = int((LoopConstant({}) >> 8) & 0xFF);", id);
                        }
                        break;
                    }
                    case ControlFlowOpcode::LoopEnd:
                    {
                        uint32_t id = cfInstr.loopEnd.loopId;
                        if (simpleControlFlow)
                        {
                            uint32_t n = openLoops.empty() ? 0 : openLoops.back();
                            if (!openLoops.empty())
                                openLoops.pop_back();
                            indent();
                            println("aL += int(loopConstant{} << 8) >> 24;", n);
                            --indentation;
                            indent();
                            out += "}\n";
                        }
                        else
                        {
                            println("\t\t\taL += int(LoopConstant({}) << 8) >> 24;", id);
                            println("\t\t\tif (++loopIteration < (LoopConstant({}) & 0xFF))", id);
                            println("\t\t\t{{\n\t\t\t\tpc = {};\n\t\t\t\tcontinue;\n\t\t\t}}", uint32_t(cfInstr.loopEnd.address));
                        }
                        break;
                    }
                    case ControlFlowOpcode::CondJmp:
                    {
                        if (cfInstr.condJmp.isUnconditional)
                        {
                            println("\t\t\tpc = {};\n\t\t\tcontinue;", uint32_t(cfInstr.condJmp.address));
                        }
                        else
                        {
                            // Simple mode emits the skipped block as "if (!jump)".
                            bool jumpIf = cfInstr.condJmp.condition;
                            bool emitIf = simpleControlFlow ? !jumpIf : jumpIf;
                            indent();
                            if (cfInstr.condJmp.isPredicated)
                                println("if ({}p0)", emitIf ? "" : "!");
                            else
                                println("if (BoolConstant({}) == {})", uint32_t(cfInstr.condJmp.boolAddress),
                                        emitIf ? "true" : "false");
                            if (simpleControlFlow)
                            {
                                indent();
                                out += "{\n";
                                ++indentation;
                            }
                            else
                            {
                                println("\t\t\t{{\n\t\t\t\tpc = {};\n\t\t\t\tcontinue;\n\t\t\t}}",
                                        uint32_t(cfInstr.condJmp.address));
                            }
                        }
                        break;
                    }
                    default:
                        break;
                    }

                    if (count)
                    {
                        if (!clauseCondition.empty())
                        {
                            indent();
                            println("if ({})", clauseCondition);
                            indent();
                            out += "{\n";
                            ++indentation;
                        }

                        const be<uint32_t>* instructionCode = code + address * 3;
                        for (uint32_t i = 0; i < count; i++)
                        {
                            union
                            {
                                VertexFetchInstruction vertexFetch;
                                TextureFetchInstruction textureFetch;
                                AluInstruction alu;
                                struct
                                {
                                    uint32_t w0, w1, w2;
                                };
                            };
                            if ((address + i) * 3 + 3 > programDwords)
                                break;
                            w0 = instructionCode[0];
                            w1 = instructionCode[1];
                            w2 = instructionCode[2];
                            if (sequence & 0x1)
                            {
                                if (vertexFetch.opcode == FetchOpcode::VertexFetch)
                                    Vfetch(vertexFetch);
                                else
                                    Tfetch(textureFetch);
                            }
                            else
                            {
                                Alu(alu);
                            }
                            sequence >>= 2;
                            instructionCode += 3;
                        }

                        if (!clauseCondition.empty())
                        {
                            --indentation;
                            indent();
                            out += "}\n";
                        }
                    }

                    if (shouldReturn)
                    {
                        if (simpleControlFlow)
                        {
                            emitEpilogue();
                            indent();
                            out += "return;\n";
                        }
                        else
                        {
                            out += "\t\t\tbreak;\n"; // the epilogue follows the dispatch loop
                        }
                    }
                }
            }

            if (!simpleControlFlow)
                out += "\t\t\tbreak;\n\t\t}\n\t\tbreak;\n\t}\n";
            // Programs normally end in an exec-end; finish anyway if not.
            indentation = 1;
            emitEpilogue();
            out += "}\n";
            return true;
        }
    }

    bool Translate(const uint8_t* container, size_t containerSize, const uint8_t* program, uint32_t programDwords,
                   Translation& out, const Linkage* linkage)
    {
        Translator t;
        t.result = &out;
        t.linkage = linkage;
        bool ok = t.Run(container, containerSize, program, programDwords);
        out.hlsl = std::move(t.out);
        return ok;
    }

    bool ContainerLinkage(const uint8_t* container, size_t containerSize, Linkage& out)
    {
        out = {};
        if (containerSize < sizeof(ShaderContainer))
            return false;
        auto* c = reinterpret_cast<const ShaderContainer*>(container);
        if ((c->flags & 0xFFFFFF00) != 0x102A1100 || c->shaderOffset >= containerSize)
            return false;
        bool pixel = (c->flags & 1) == 0;
        auto* shader = reinterpret_cast<const Shader*>(container + c->shaderOffset);
        uint32_t count = (shader->interpolatorInfo >> 5) & 0x1F;
        for (uint32_t i = 0; i < count; i++)
        {
            union
            {
                Interpolator interpolator;
                uint32_t value;
            };
            size_t entry;
            if (pixel)
                entry = c->shaderOffset + offsetof(PixelShader, interpolators) + i * 4;
            else
            {
                auto* vs = reinterpret_cast<const VertexShader*>(shader);
                entry = c->shaderOffset + offsetof(VertexShader, vertexElementsAndInterpolators) +
                        (vs->field18 + vs->vertexElementCount + i) * 4;
            }
            if (entry + 4 > containerSize)
                return false;
            value = reinterpret_cast<const be<uint32_t>*>(container + entry)->get();
            // Pixel shader entries name the register they load; vertex shader entries
            // are in export order.
            uint32_t slot = pixel ? uint32_t(interpolator.reg) : i;
            if (slot >= 16)
                continue;
            out.valid[slot] = true;
            out.usage[slot] = uint8_t(interpolator.usage);
            out.usageIndex[slot] = uint8_t(interpolator.usageIndex);
        }
        return true;
    }

    bool TranslateRaw(bool isPixel, const uint8_t* program, uint32_t programDwords, Translation& out, const Linkage* linkage)
    {
        // Pixel shaders: a first pass with every output declared finds the ones
        // the program exports; the second declares exactly those.
        uint32_t outputs = 0;
        if (isPixel)
        {
            Translation probe;
            Translator t;
            t.result = &probe;
            t.isPixel = true;
            t.linkage = linkage;
            t.psOutputs = PIXEL_SHADER_OUTPUT_COLOR0 | PIXEL_SHADER_OUTPUT_COLOR1 | PIXEL_SHADER_OUTPUT_COLOR2 |
                          PIXEL_SHADER_OUTPUT_COLOR3 | PIXEL_SHADER_OUTPUT_DEPTH;
            if (!t.Run(nullptr, 0, program, programDwords))
            {
                out = std::move(probe);
                return false;
            }
            for (uint32_t i = 0; i < 4; i++)
                if (probe.colorOutputs & (1u << i))
                    outputs |= PIXEL_SHADER_OUTPUT_COLOR0 << i;
            if (probe.writesDepth)
                outputs |= PIXEL_SHADER_OUTPUT_DEPTH;
        }
        Translator t;
        t.result = &out;
        t.isPixel = isPixel;
        t.psOutputs = outputs;
        t.linkage = linkage;
        bool ok = t.Run(nullptr, 0, program, programDwords);
        out.hlsl = std::move(t.out);
        return ok;
    }
}
