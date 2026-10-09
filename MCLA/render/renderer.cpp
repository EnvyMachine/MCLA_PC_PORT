#include "renderer.h"
#include "descriptors.h"
#include "device.h"
#include "screenshot.h"
#include "shader_cache.h"
#include "shared_memory.h"
#include "../runtime/memory_watch.h"
#include "texture_cache.h"
#include "upload.h"
#include "xenos/system_constants.h"
#include "../gpu/command_processor.h"
#include "../gpu/shader_registry.h"
#include "../runtime/guest.h"
#include "../runtime/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace render
{
    namespace
    {
        // ---- Registers ----
        enum : uint32_t
        {
            RB_SURFACE_INFO = 0x2000, RB_COLOR_INFO = 0x2001, RB_DEPTH_INFO = 0x2002,
            PA_SC_SCREEN_SCISSOR_TL = 0x200E, PA_SC_SCREEN_SCISSOR_BR = 0x200F,
            PA_SC_WINDOW_OFFSET = 0x2080, PA_SC_WINDOW_SCISSOR_TL = 0x2081, PA_SC_WINDOW_SCISSOR_BR = 0x2082,
            VGT_MAX_VTX_INDX = 0x2100, VGT_MIN_VTX_INDX = 0x2101, VGT_INDX_OFFSET = 0x2102,
            RB_COLOR_MASK = 0x2104, RB_BLEND_RED = 0x2105,
            RB_STENCILREFMASK_BF = 0x210C, RB_STENCILREFMASK = 0x210D, RB_ALPHA_REF = 0x210E,
            PA_CL_VPORT_XSCALE = 0x210F, PA_CL_VPORT_XOFFSET = 0x2110, PA_CL_VPORT_YSCALE = 0x2111,
            PA_CL_VPORT_YOFFSET = 0x2112, PA_CL_VPORT_ZSCALE = 0x2113, PA_CL_VPORT_ZOFFSET = 0x2114,
            RB_DEPTHCONTROL = 0x2200, RB_COLORCONTROL = 0x2202, PA_CL_CLIP_CNTL = 0x2204,
            PA_SU_SC_MODE_CNTL = 0x2205, PA_CL_VTE_CNTL = 0x2206, RB_MODECONTROL = 0x2208,
            PA_SU_VTX_CNTL = 0x2302,
            RB_COPY_CONTROL = 0x2318, RB_COPY_DEST_BASE = 0x2319, RB_COPY_DEST_PITCH = 0x231A,
            RB_COPY_DEST_INFO = 0x231B, RB_DEPTH_CLEAR = 0x231D, RB_COLOR_CLEAR = 0x231E, RB_COLOR_CLEAR_LO = 0x231F,
            SHADER_CONSTANT_000_X = 0x4000, SHADER_CONSTANT_256_X = 0x4400, SHADER_CONSTANT_FETCH_00_0 = 0x4800,
            SHADER_CONSTANT_BOOL_000_031 = 0x4900, SHADER_CONSTANT_LOOP_00 = 0x4908,
        };
        constexpr uint32_t kColorInfo[4] = { 0x2001, 0x2003, 0x2004, 0x2005 };
        constexpr uint32_t kBlendControl[4] = { 0x2201, 0x2209, 0x220A, 0x220B };

        uint32_t Reg(uint32_t index) { return gpu::ReadRegister(index); }
        float RegF(uint32_t index)
        {
            uint32_t v = Reg(index);
            float f;
            memcpy(&f, &v, 4);
            return f;
        }
        int32_t SignExtend(uint32_t v, uint32_t bits) { return int32_t(v << (32 - bits)) >> (32 - bits); }

        // Copies count guest registers starting at index into dst (host order).
        void ReadRegisters(uint32_t index, uint32_t count, uint32_t* dst)
        {
            const uint32_t* src = reinterpret_cast<const uint32_t*>(g_base + 0x7FC80000 + index * 4);
            for (uint32_t i = 0; i < count; i++)
                dst[i] = __builtin_bswap32(src[i]);
        }

        // A float constant bank (256 registers) as a constant buffer. Consecutive
        // draws mostly share constants, so an unchanged bank reuses the frame's
        // previous upload.
        struct ConstantBank
        {
            uint32_t raw[1024]; // guest order, as last uploaded
            D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
            uint64_t frame = ~0ull;
        };
        ConstantBank s_vsBank, s_psBank;

        D3D12_GPU_VIRTUAL_ADDRESS UploadConstantBank(ConstantBank& bank, uint32_t firstRegister)
        {
            const uint32_t* src = reinterpret_cast<const uint32_t*>(g_base + 0x7FC80000 + firstRegister * 4);
            if (bank.frame == FrameNumber() && memcmp(bank.raw, src, sizeof(bank.raw)) == 0)
                return bank.gpu;
            memcpy(bank.raw, src, sizeof(bank.raw));
            UploadSpace space = AllocateUpload(sizeof(bank.raw));
            uint32_t* dst = reinterpret_cast<uint32_t*>(space.cpu);
            for (uint32_t i = 0; i < 1024; i++)
                dst[i] = __builtin_bswap32(bank.raw[i]);
            bank.gpu = space.gpu;
            bank.frame = FrameNumber();
            return bank.gpu;
        }

        // ---- Statistics ----
        struct Stats
        {
            uint64_t draws, drawsIssued, skippedShaderUnknown, skippedShaderPending, skippedShaderFailed,
                skippedPrimitive, resolves, pipelines;
            uint64_t barriers, mirrorCommits; // GPU-side pipeline drains
            int64_t drawTicks; // QPC ticks spent in Draw
        } s_stats;

        // Adds the lifetime of the scope to a QPC tick counter.
        struct ScopeTimer
        {
            int64_t& total;
            LARGE_INTEGER start;
            explicit ScopeTimer(int64_t& t) : total(t) { QueryPerformanceCounter(&start); }
            ~ScopeTimer()
            {
                LARGE_INTEGER end;
                QueryPerformanceCounter(&end);
                total += end.QuadPart - start.QuadPart;
            }
        };
        LARGE_INTEGER s_lastStatsTime{};
        // Hitches: the longest swap-to-swap interval in the stats window.
        LARGE_INTEGER s_lastSwapTime{};
        double s_slowestFrameMs = 0;
        uint32_t s_slowFrames = 0;
        // Per-frame costs that cause hitches (logged for frames over 50 ms).
        int64_t s_framePsoTicks = 0, s_frameDrawTicks = 0;
        uint32_t s_framePsos = 0;
        // Draw sections (MCLA_LOG_HITCHES).
        enum Section { kSecResolve, kSecShaders, kSecTargets, kSecPipeline, kSecMirror, kSecBindings, kSecIssue, kSecCount };
        int64_t s_sectionTicks[kSecCount] = {};
        struct SectionClock
        {
            LARGE_INTEGER last;
            SectionClock() { QueryPerformanceCounter(&last); }
            void Lap(Section s)
            {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                s_sectionTicks[s] += now.QuadPart - last.QuadPart;
                last = now;
            }
        };

        // MCLA_DRAW_LOG=<frame>: log every draw and resolve of that frame.
        uint64_t s_swapCount = 0;
        // MCLA_DRAW_LOG=<frame>[-<last frame>]: log every draw of those frames.
        bool LogDraws()
        {
            static const std::pair<uint64_t, uint64_t> frames = [] {
                char v[32];
                unsigned long long from = ~0ull, to = 0;
                if (GetEnvironmentVariableA("MCLA_DRAW_LOG", v, sizeof(v)) > 0 && sscanf(v, "%llu-%llu", &from, &to) < 2)
                    to = from;
                return std::pair<uint64_t, uint64_t>(from, to);
            }();
            return s_swapCount >= frames.first && s_swapCount <= frames.second;
        }

        // ---- Root signature and built-in shaders ----
        ComPtr<ID3D12RootSignature> s_rootSignature;
        ComPtr<ID3D12RootSignature> s_blitRootSignature;
        ComPtr<ID3D12RootSignature> s_resolveRootSignature;
        ComPtr<ID3D12PipelineState> s_blitPipeline;
        std::vector<uint8_t> s_rectListGs, s_quadListGs;
        ComPtr<ID3D12Resource> s_placeholders[3];
        uint32_t s_nullRtv = 0;

        enum RootParameter : uint32_t
        {
            kRootVSConstants, kRootPSConstants, kRootSystemConstants, kRootMemory, kRootTextures, kRootSamplers
        };

        void Fatal(const char* what, HRESULT hr)
        {
            LOGI("render: %s failed (%08lX)", what, hr);
            LogFlush();
            ExitProcess(1);
        }

        ComPtr<ID3D12RootSignature> CreateRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc)
        {
            ComPtr<ID3DBlob> blob, error;
            HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
            if (FAILED(hr))
            {
                LOGI("render: root signature: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
                Fatal("D3D12SerializeVersionedRootSignature", hr);
            }
            ComPtr<ID3D12RootSignature> rs;
            hr = Device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
            if (FAILED(hr))
                Fatal("CreateRootSignature", hr);
            return rs;
        }

        void CreateRootSignatures()
        {
            D3D12_DESCRIPTOR_RANGE1 textureRanges[3] = {};
            for (uint32_t i = 0; i < 3; i++)
            {
                textureRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                textureRanges[i].NumDescriptors = UINT_MAX;
                textureRanges[i].BaseShaderRegister = 0;
                textureRanges[i].RegisterSpace = 1 + i;
                textureRanges[i].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
                textureRanges[i].OffsetInDescriptorsFromTableStart = 0; // the same heap, viewed as 2D, 3D and cube arrays
            }
            D3D12_DESCRIPTOR_RANGE1 samplerRange{};
            samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            samplerRange.NumDescriptors = UINT_MAX;
            samplerRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

            D3D12_ROOT_PARAMETER1 params[6] = {};
            for (uint32_t i = 0; i < 3; i++)
            {
                params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                params[i].Descriptor.ShaderRegister = i;
                params[i].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
                params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            }
            params[kRootMemory].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            params[kRootMemory].Descriptor.ShaderRegister = 0;
            params[kRootMemory].Descriptor.RegisterSpace = 0;
            params[kRootMemory].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
            params[kRootMemory].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[kRootTextures].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[kRootTextures].DescriptorTable.NumDescriptorRanges = 3;
            params[kRootTextures].DescriptorTable.pDescriptorRanges = textureRanges;
            params[kRootTextures].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[kRootSamplers].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[kRootSamplers].DescriptorTable.NumDescriptorRanges = 1;
            params[kRootSamplers].DescriptorTable.pDescriptorRanges = &samplerRange;
            params[kRootSamplers].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
            desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
            desc.Desc_1_1.NumParameters = 6;
            desc.Desc_1_1.pParameters = params;
            char so[4];
            if (GetEnvironmentVariableA("MCLA_SO_VS", so, sizeof(so)) > 0) // stream-output capture (debugging)
                desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
            s_rootSignature = CreateRootSignature(desc);

            // Present blit: one SRV + a static linear sampler.
            D3D12_DESCRIPTOR_RANGE1 blitRange{};
            blitRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            blitRange.NumDescriptors = 1;
            blitRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
            D3D12_ROOT_PARAMETER1 blitParam{};
            blitParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            blitParam.DescriptorTable.NumDescriptorRanges = 1;
            blitParam.DescriptorTable.pDescriptorRanges = &blitRange;
            blitParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC blitDesc{};
            blitDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
            blitDesc.Desc_1_1.NumParameters = 1;
            blitDesc.Desc_1_1.pParameters = &blitParam;
            blitDesc.Desc_1_1.NumStaticSamplers = 1;
            blitDesc.Desc_1_1.pStaticSamplers = &sampler;
            s_blitRootSignature = CreateRootSignature(blitDesc);

            // Shader resolves: 8 root constants (b0) + one SRV.
            D3D12_ROOT_PARAMETER1 resolveParams[2] = {};
            resolveParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            resolveParams[0].Constants.Num32BitValues = 8;
            resolveParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            resolveParams[1] = blitParam;
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC resolveDesc{};
            resolveDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
            resolveDesc.Desc_1_1.NumParameters = 2;
            resolveDesc.Desc_1_1.pParameters = resolveParams;
            s_resolveRootSignature = CreateRootSignature(resolveDesc);
        }

        // Vertex layout every translated shader passes between stages.
        constexpr const char kStageVertex[] = R"(
struct V
{
    float4 pos : SV_Position;
    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
    float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7;
    float4 t8 : TEXCOORD8; float4 t9 : TEXCOORD9; float4 t10 : TEXCOORD10; float4 t11 : TEXCOORD11;
    float4 t12 : TEXCOORD12; float4 t13 : TEXCOORD13; float4 t14 : TEXCOORD14; float4 t15 : TEXCOORD15;
    float4 c0 : COLOR0; float4 c1 : COLOR1;
};
V Extrapolate(V a, V b, V c) // a + b - c, component-wise
{
    V r;
    r.pos = a.pos + b.pos - c.pos;
    r.t0 = a.t0 + b.t0 - c.t0; r.t1 = a.t1 + b.t1 - c.t1; r.t2 = a.t2 + b.t2 - c.t2; r.t3 = a.t3 + b.t3 - c.t3;
    r.t4 = a.t4 + b.t4 - c.t4; r.t5 = a.t5 + b.t5 - c.t5; r.t6 = a.t6 + b.t6 - c.t6; r.t7 = a.t7 + b.t7 - c.t7;
    r.t8 = a.t8 + b.t8 - c.t8; r.t9 = a.t9 + b.t9 - c.t9; r.t10 = a.t10 + b.t10 - c.t10; r.t11 = a.t11 + b.t11 - c.t11;
    r.t12 = a.t12 + b.t12 - c.t12; r.t13 = a.t13 + b.t13 - c.t13; r.t14 = a.t14 + b.t14 - c.t14; r.t15 = a.t15 + b.t15 - c.t15;
    r.c0 = a.c0 + b.c0 - c.c0; r.c1 = a.c1 + b.c1 - c.c1;
    return r;
}
)";

        // Rectangle lists: three corners per rectangle; the fourth is opposite the
        // corner where the two shorter edges meet.
        constexpr const char kRectListGs[] = R"(
[maxvertexcount(4)]
void main(triangle V v[3], inout TriangleStream<V> s)
{
    float2 p0 = v[0].pos.xy / v[0].pos.w, p1 = v[1].pos.xy / v[1].pos.w, p2 = v[2].pos.xy / v[2].pos.w;
    float d01 = dot(p0 - p1, p0 - p1), d02 = dot(p0 - p2, p0 - p2), d12 = dot(p1 - p2, p1 - p2);
    uint corner = (d12 >= d01 && d12 >= d02) ? 0 : (d02 >= d01 ? 1 : 2);
    V c = v[corner], a = v[(corner + 1) % 3], b = v[(corner + 2) % 3];
    s.Append(a);
    s.Append(c);
    s.Append(Extrapolate(a, b, c));
    s.Append(b);
}
)";

        // Quad lists: four vertices per quad, as a strip 0 1 3 2.
        constexpr const char kQuadListGs[] = R"(
[maxvertexcount(4)]
void main(lineadj V v[4], inout TriangleStream<V> s)
{
    s.Append(v[0]);
    s.Append(v[1]);
    s.Append(v[3]);
    s.Append(v[2]);
}
)";

        constexpr const char kBlitVs[] = R"(
void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

        constexpr const char kBlitPs[] = R"(
Texture2D<float4> t : register(t0);
SamplerState s : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return float4(t.Sample(s, uv).rgb, 1.0);
}
)";

        // Resolves that a copy cannot do: averaging the samples of a multisampled
        // EDRAM surface (stored as its sample grid), depth to R32_FLOAT (which
        // takes the first sample, as the hardware does), and color conversions to
        // the destination: its exponent bias and the range of fixed-point formats.
        constexpr const char kResolvePs[] = R"(
Texture2D<float4> g_Source : register(t0);
cbuffer Params : register(b0)
{
    int2 g_SourceOrigin; // in samples
    int2 g_Scale;        // samples per pixel
    int2 g_DestOrigin;
    uint g_FirstSampleOnly;
    uint g_Convert;      // bits 0-7: exponent bias (signed), bit 8: clamp to 0..1
};
float4 main(float4 pos : SV_Position) : SV_Target
{
    int2 base = (int2(pos.xy) - g_DestOrigin) * g_Scale + g_SourceOrigin;
    if (g_FirstSampleOnly != 0)
        return g_Source.Load(int3(base, 0));
    float4 sum = 0.0;
    for (int y = 0; y < g_Scale.y; y++)
        for (int x = 0; x < g_Scale.x; x++)
            sum += g_Source.Load(int3(base + int2(x, y), 0));
    float4 color = sum / float(g_Scale.x * g_Scale.y);
    color *= exp2(float(int(g_Convert << 24) >> 24));
    if (g_Convert & 0x100)
        color = saturate(color);
    return color;
}
)";
        constexpr const char kDebugPs[] = R"(
float4 main(float4 pos : SV_Position) : SV_Target0 { return float4(1.0, 0.0, 1.0, 1.0); }
)";
        std::vector<uint8_t> s_blitVs, s_resolvePs, s_debugPs;
        std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> s_resolvePipelines; // by RTV format

        ID3D12PipelineState* ResolvePipeline(DXGI_FORMAT format)
        {
            auto& slot = s_resolvePipelines[uint32_t(format)];
            if (slot)
                return slot.Get();
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = s_resolveRootSignature.Get();
            pd.VS = { s_blitVs.data(), s_blitVs.size() };
            pd.PS = { s_resolvePs.data(), s_resolvePs.size() };
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = format;
            pd.SampleDesc.Count = 1;
            if (FAILED(Device()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&slot))))
            {
                LOGI("render: could not create a resolve pipeline for format %d", int(format));
                return nullptr;
            }
            return slot.Get();
        }

        bool CreateBuiltinShaders()
        {
            std::string rect = std::string(kStageVertex) + kRectListGs;
            std::string quad = std::string(kStageVertex) + kQuadListGs;
            std::vector<uint8_t> blitPs;
            if (!CompileHlsl(rect.c_str(), L"gs_6_0", s_rectListGs) || !CompileHlsl(quad.c_str(), L"gs_6_0", s_quadListGs) ||
                !CompileHlsl(kBlitVs, L"vs_6_0", s_blitVs) || !CompileHlsl(kBlitPs, L"ps_6_0", blitPs) ||
                !CompileHlsl(kResolvePs, L"ps_6_0", s_resolvePs) || !CompileHlsl(kDebugPs, L"ps_6_0", s_debugPs))
                return false;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = s_blitRootSignature.Get();
            pd.VS = { s_blitVs.data(), s_blitVs.size() };
            pd.PS = { blitPs.data(), blitPs.size() };
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.RasterizerState.DepthClipEnable = TRUE;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pd.SampleDesc.Count = 1;
            HRESULT hr = Device()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&s_blitPipeline));
            if (FAILED(hr))
                Fatal("CreateGraphicsPipelineState(blit)", hr);
            return true;
        }

        void CreatePlaceholders()
        {
            // 1x1 white textures for unbound fetch slots: 2D (index 0), 3D (1), cube (2).
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC desc{};
            desc.Width = 1;
            desc.Height = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            for (uint32_t i = 0; i < 3; i++)
            {
                desc.Dimension = i == 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                desc.DepthOrArraySize = i == 2 ? 6 : 1;
                HRESULT hr = Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                               nullptr, IID_PPV_ARGS(&s_placeholders[i]));
                if (FAILED(hr))
                    Fatal("CreateCommittedResource(placeholder)", hr);
            }
            ID3D12GraphicsCommandList* list = CommandList();
            UploadSpace white = AllocateUpload(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * 6, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            for (uint32_t face = 0; face < 6; face++)
                memset(white.cpu + face * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, 0xFF, 4);
            for (uint32_t i = 0; i < 3; i++)
            {
                for (uint32_t face = 0; face < (i == 2 ? 6u : 1u); face++)
                {
                    D3D12_TEXTURE_COPY_LOCATION dst{ s_placeholders[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                    dst.SubresourceIndex = face;
                    D3D12_TEXTURE_COPY_LOCATION src{ white.resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                    src.PlacedFootprint.Offset = white.offset + face * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
                    src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };
                    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                }
                D3D12_RESOURCE_BARRIER b{};
                b.Transition.pResource = s_placeholders[i].Get();
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                list->ResourceBarrier(1, &b);

                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.ViewDimension = i == 0 ? D3D12_SRV_DIMENSION_TEXTURE2D
                                           : (i == 1 ? D3D12_SRV_DIMENSION_TEXTURE3D : D3D12_SRV_DIMENSION_TEXTURECUBE);
                srv.Texture2D.MipLevels = 1; // same layout for 3D and cube
                Device()->CreateShaderResourceView(s_placeholders[i].Get(), &srv, CpuHandle(HeapKind::kView, i));
            }
            D3D12_SAMPLER_DESC sd{};
            sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            Device()->CreateSampler(&sd, CpuHandle(HeapKind::kSampler, 0));

            s_nullRtv = AllocateDescriptor(HeapKind::kRtv);
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            Device()->CreateRenderTargetView(nullptr, &rtv, CpuHandle(HeapKind::kRtv, s_nullRtv));
        }

        // ---- Render targets (EDRAM surfaces) ----

        // An EDRAM surface, stored as its grid of samples: with 2x MSAA a pixel is
        // 1x2 samples and with 4x 2x2, as the EDRAM lays them out. Different views
        // of the same EDRAM therefore share one host texture - Direct3D clears a 1x
        // surface by drawing it as a half-size 4x one, and MCLA draws its scene as
        // 2_10_10_10_FLOAT and resolves it through 2_10_10_10_FLOAT_AS_16_16_16_16
        // (both map to one host format). Surfaces are keyed by EDRAM base, pitch in
        // samples and host format, and keep one RTV per view format.
        struct RenderTarget
        {
            ComPtr<ID3D12Resource> resource;
            DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN; // typeless family
            DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;     // RTV/DSV format of the guest format last bound
            uint32_t width = 0, height = 0;                   // in samples
            bool depth = false;
            uint32_t view = 0; // RTV (for viewFormat) or DSV index
            std::vector<std::pair<DXGI_FORMAT, uint32_t>> rtvs;
            uint32_t srv = 0; // for resolves through a shader, created on first use
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
            uint32_t edramBase = 0, pitchTiles = 0;
            uint32_t keptTileEnd = 0;          // end of the EDRAM tiles its resolved content occupies
            uint64_t lastDrawFrame = ~0ull;
        };
        std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> s_renderTargets;

        // EDRAM sharing. On the console every surface is a view of the same EDRAM, so
        // a pass using some tiles destroys what another surface kept there; here each
        // surface is its own texture and keeps its contents. MCLA's minimap relies on
        // the console behavior: it clears only a border strip and draws its streets
        // onto whatever earlier passes (cleared) left in its tiles, so here every
        // frame's streets stayed and smeared into one street-colored area while
        // driving. A resolve marks the tiles its surface's content occupies; a color
        // surface drawn for the first time in a frame whose tiles another surface has
        // been resolved from since starts from zero instead of its stale contents.
        constexpr uint32_t kEdramTiles = 2048;
        RenderTarget* s_tileOwner[kEdramTiles] = {};
        void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES& state,
                        D3D12_RESOURCE_STATES next);

        void ClaimTiles(RenderTarget& rt, uint32_t sampleRows)
        {
            uint32_t end = std::min(kEdramTiles, rt.edramBase + (sampleRows + 15) / 16 * rt.pitchTiles);
            for (uint32_t t = rt.edramBase; t < end; t++)
                s_tileOwner[t] = &rt;
            rt.keptTileEnd = std::max(rt.keptTileEnd, end);
        }

        void DiscardIfOverwritten(ID3D12GraphicsCommandList* list, RenderTarget& rt)
        {
            if (rt.depth || rt.lastDrawFrame == FrameNumber())
                return;
            rt.lastDrawFrame = FrameNumber();
            bool stale = false;
            for (uint32_t t = rt.edramBase; t < rt.keptTileEnd && !stale; t++)
                stale = s_tileOwner[t] && s_tileOwner[t] != &rt;
            if (!stale)
                return;
            rt.keptTileEnd = 0;
            Transition(list, rt.resource.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const float zero[4] = {};
            list->ClearRenderTargetView(CpuHandle(HeapKind::kRtv, rt.view), zero, 0, nullptr);
        }

        // Samples per pixel along X and Y for RB_SURFACE_INFO's msaa_samples.
        uint32_t MsaaScaleX(uint32_t msaa) { return msaa >= 2 ? 2 : 1; }
        uint32_t MsaaScaleY(uint32_t msaa) { return msaa >= 1 ? 2 : 1; }

        bool IsColorFormat64bpp(uint32_t format) { return format == 5 || format == 7 || format == 15; }

        void ColorFormats(uint32_t format, DXGI_FORMAT& resource, DXGI_FORMAT& view)
        {
            switch (format)
            {
            case 0: resource = DXGI_FORMAT_R8G8B8A8_TYPELESS; view = DXGI_FORMAT_R8G8B8A8_UNORM; break;
            case 1: resource = DXGI_FORMAT_R8G8B8A8_TYPELESS; view = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break; // PWL gamma ~ sRGB
            case 2: case 10: resource = DXGI_FORMAT_R10G10B10A2_TYPELESS; view = DXGI_FORMAT_R10G10B10A2_UNORM; break;
            case 4: case 6: resource = DXGI_FORMAT_R16G16_TYPELESS; view = DXGI_FORMAT_R16G16_FLOAT; break;
            case 14: resource = DXGI_FORMAT_R32_TYPELESS; view = DXGI_FORMAT_R32_FLOAT; break;
            case 15: resource = DXGI_FORMAT_R32G32_TYPELESS; view = DXGI_FORMAT_R32G32_FLOAT; break;
            default: // 2_10_10_10_FLOAT, 16_16_16_16 (fixed -32..32), 16_16_16_16_FLOAT
                resource = DXGI_FORMAT_R16G16B16A16_TYPELESS; view = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
            }
        }

        // A 20e4 float depth value (D24FS8's 24 bits) as a float: 4-bit exponent
        // biased by 15, 20-bit mantissa, denormals.
        float Float20e4To32(uint32_t f24)
        {
            f24 &= 0xFFFFFF;
            if (!f24)
                return 0.0f;
            uint32_t mantissa = f24 & 0xFFFFF, exponent = f24 >> 20;
            if (!exponent)
            {
                uint32_t shift = uint32_t(__builtin_clz(mantissa)) - (32 - 20) + 1;
                exponent = 1 - shift;
                mantissa = (mantissa << shift) & 0xFFFFF;
            }
            uint32_t bits = ((exponent + 112) << 23) | (mantissa << 3);
            float f;
            memcpy(&f, &bits, 4);
            return f;
        }

        // A 7e3 float (2_10_10_10_FLOAT's color components) as a float: 3-bit
        // exponent biased by 3, 7-bit mantissa, denormals.
        float Float7e3To32(uint32_t f10)
        {
            f10 &= 0x3FF;
            if (!f10)
                return 0.0f;
            uint32_t mantissa = f10 & 0x7F, exponent = f10 >> 7;
            if (!exponent)
            {
                uint32_t shift = uint32_t(__builtin_clz(mantissa)) - (32 - 7) + 1;
                exponent = 1 - shift;
                mantissa = (mantissa << shift) & 0x7F;
            }
            uint32_t bits = ((exponent + 124) << 23) | (mantissa << 16);
            float f;
            memcpy(&f, &bits, 4);
            return f;
        }

        // RB_COLOR_CLEAR (and RB_COLOR_CLEAR_LO, the low half of 64-bit formats) are
        // the raw EDRAM bits of the clear color in the surface's own format. MCLA
        // clears its 2_10_10_10_FLOAT scene to 0x21986619, an HDR gray of 2.39 with
        // alpha 0 - not a dark teal with alpha 0x21, as the bytes read as 8_8_8_8.
        void DecodeColorClear(uint32_t guestFormat, uint32_t hi, uint32_t lo, float out[4])
        {
            uint64_t v64 = uint64_t(hi) << 32 | lo;
            auto half = [](uint32_t h) {
                uint32_t bits = h & 0xFFFF;
                uint32_t sign = bits >> 15, exp = (bits >> 10) & 0x1F, mant = bits & 0x3FF;
                float r = exp == 0 ? std::ldexp(float(mant), -24)
                                   : exp == 31 ? (mant ? NAN : INFINITY) : std::ldexp(float(mant | 0x400), int(exp) - 25);
                return sign ? -r : r;
            };
            auto fixed16 = [](uint32_t h) { return float(int16_t(h & 0xFFFF)) * (32.0f / 32767.0f); };
            switch (guestFormat)
            {
            case 0: case 1: // 8_8_8_8 (and _GAMMA: the bytes are gamma-encoded; the sRGB view re-encodes)
                for (int i = 0; i < 4; i++)
                {
                    float c = float((hi >> (i * 8)) & 0xFF) / 255.0f;
                    if (guestFormat == 1 && i < 3)
                        c = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
                    out[i] = c;
                }
                break;
            case 2: case 10: // 2_10_10_10 (unorm)
                for (int i = 0; i < 3; i++)
                    out[i] = float((hi >> (i * 10)) & 0x3FF) / 1023.0f;
                out[3] = float(hi >> 30) / 3.0f;
                break;
            case 3: case 12: // 2_10_10_10_FLOAT
                for (int i = 0; i < 3; i++)
                    out[i] = Float7e3To32(hi >> (i * 10));
                out[3] = float(hi >> 30) / 3.0f;
                break;
            case 4: // 16_16 (fixed point, -32..32)
                out[0] = fixed16(hi);
                out[1] = fixed16(hi >> 16);
                out[2] = 0.0f;
                out[3] = 1.0f;
                break;
            case 5: // 16_16_16_16 (fixed point)
                for (int i = 0; i < 4; i++)
                    out[i] = fixed16(uint32_t(v64 >> (i * 16)));
                break;
            case 6: // 16_16_FLOAT
                out[0] = half(hi);
                out[1] = half(hi >> 16);
                out[2] = 0.0f;
                out[3] = 1.0f;
                break;
            case 7: // 16_16_16_16_FLOAT
                for (int i = 0; i < 4; i++)
                    out[i] = half(uint32_t(v64 >> (i * 16)));
                break;
            case 14: // 32_FLOAT
                memcpy(&out[0], &hi, 4);
                out[1] = out[2] = 0.0f;
                out[3] = 1.0f;
                break;
            case 15: // 32_32_FLOAT
                memcpy(&out[0], &lo, 4);
                memcpy(&out[1], &hi, 4);
                out[2] = 0.0f;
                out[3] = 1.0f;
                break;
            default:
                for (int i = 0; i < 4; i++)
                    out[i] = float((hi >> (i * 8)) & 0xFF) / 255.0f;
                break;
            }
        }

        // Selects (creating if needed) the RTV of rt for a color view format.
        void SelectColorView(RenderTarget& rt, DXGI_FORMAT viewFormat)
        {
            rt.viewFormat = viewFormat;
            for (const auto& [format, rtv] : rt.rtvs)
            {
                if (format == viewFormat)
                {
                    rt.view = rtv;
                    return;
                }
            }
            rt.view = AllocateDescriptor(HeapKind::kRtv);
            D3D12_RENDER_TARGET_VIEW_DESC desc{};
            desc.Format = viewFormat;
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            Device()->CreateRenderTargetView(rt.resource.Get(), &desc, CpuHandle(HeapKind::kRtv, rt.view));
            rt.rtvs.push_back({ viewFormat, rt.view });
        }

        RenderTarget* GetRenderTarget(uint32_t base, uint32_t pitch, uint32_t msaa, uint32_t format, bool depth)
        {
            if (!pitch)
                return nullptr;
            DXGI_FORMAT resourceFormat, viewFormat = DXGI_FORMAT_UNKNOWN;
            if (depth)
            {
                // Both guest depth formats (24-bit unorm and 20e4 float) are stored as
                // 32-bit float depth: in EDRAM they share the same bits - stencil in
                // particular - and games switch formats on one surface (MCLA writes
                // stencil through D24FS8 and tests it through D24S8).
                (void)format;
                resourceFormat = DXGI_FORMAT_R32G8X24_TYPELESS;
                viewFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            }
            else
            {
                ColorFormats(format, resourceFormat, viewFormat);
            }
            uint32_t samplesX = pitch * MsaaScaleX(msaa);
            uint64_t key = uint64_t(base) | uint64_t(samplesX) << 12 | uint64_t(resourceFormat) << 28 | uint64_t(depth) << 36;
            auto& slot = s_renderTargets[key];
            if (slot)
            {
                if (!depth && slot->viewFormat != viewFormat)
                    SelectColorView(*slot, viewFormat);
                return slot.get();
            }

            // Height: the sample rows that fit in the EDRAM (2048 tiles of 80x16
            // 32-bit samples) from the base.
            uint32_t pitchTiles = (samplesX + 79) / 80 * (!depth && IsColorFormat64bpp(format) ? 2 : 1);
            uint32_t rows = base < 2048 ? (2048 - base) / std::max(pitchTiles, 1u) : 1;
            uint32_t height = std::clamp(rows * 16, 16u, 4096u);

            auto rt = std::make_unique<RenderTarget>();
            rt->width = samplesX;
            rt->height = height;
            rt->depth = depth;
            rt->edramBase = base;
            rt->pitchTiles = pitchTiles;
            rt->resourceFormat = resourceFormat;
            rt->viewFormat = viewFormat;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = samplesX;
            desc.Height = height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = resourceFormat;
            desc.SampleDesc.Count = 1;
            D3D12_CLEAR_VALUE clear{};
            clear.Format = viewFormat;
            if (depth)
            {
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                clear.DepthStencil.Depth = 1.0f;
                rt->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            }
            else
            {
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                rt->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            HRESULT hr = Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, rt->state, &clear,
                                                           IID_PPV_ARGS(&rt->resource));
            if (FAILED(hr))
                Fatal("CreateCommittedResource(render target)", hr);
            if (depth)
            {
                rt->view = AllocateDescriptor(HeapKind::kDsv);
                D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
                dsv.Format = viewFormat;
                dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                Device()->CreateDepthStencilView(rt->resource.Get(), &dsv, CpuHandle(HeapKind::kDsv, rt->view));
            }
            else
            {
                SelectColorView(*rt, rt->viewFormat);
            }
            LOGI("render: %s render target at EDRAM tile %u: %ux%u samples, format %u", depth ? "depth" : "color", base,
                 samplesX, height, format);
            slot = std::move(rt);
            return slot.get();
        }

        // A shader-readable view of a render target (resolves that convert or
        // average samples): color through its non-sRGB view, depth as its depth.
        uint32_t RenderTargetSrv(RenderTarget& rt)
        {
            if (rt.srv)
                return rt.srv;
            rt.srv = AllocateDescriptor(HeapKind::kView);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            if (rt.depth)
                srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            else
                srv.Format = rt.viewFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : rt.viewFormat;
            Device()->CreateShaderResourceView(rt.resource.Get(), &srv, CpuHandle(HeapKind::kView, rt.srv));
            return rt.srv;
        }

        void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES& state,
                        D3D12_RESOURCE_STATES next)
        {
            if (state == next)
                return;
            D3D12_RESOURCE_BARRIER b{};
            b.Transition.pResource = resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = state;
            b.Transition.StateAfter = next;
            list->ResourceBarrier(1, &b);
            state = next;
            s_stats.barriers++;
        }

        // ---- Resolved textures (render-to-texture) ----

        struct ResolvedTexture
        {
            ComPtr<ID3D12Resource> resource;
            DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
            DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
            uint32_t phys = 0;
            uint32_t width = 0, height = 0;
            uint32_t srv = 0;
            uint32_t rtv = 0; // for resolves through a shader, created on first use
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
            uint64_t lastResolve = 0;      // s_resolveCount at the latest resolve into it
            uint64_t lastResolveFrame = 0; // frame of the latest resolve into it
            uint64_t lastUsedFrame = 0;    // resolved into or sampled
        };
        // Games reuse one address for resolves of different sizes (MCLA alternates
        // 64x32, 256x256 and 1280x720 at one address every frame), so each size
        // and format gets its own texture, and readers pick by size, then recency.
        std::unordered_map<uint64_t, std::unique_ptr<ResolvedTexture>> s_resolved;
        std::unordered_map<uint32_t, std::vector<ResolvedTexture*>> s_resolvedByAddress;
        uint64_t s_resolveCount = 0;

        // The texture resolved at phys that best matches width x height (0: any).
        ResolvedTexture* FindResolved(uint32_t phys, uint32_t width, uint32_t height)
        {
            auto it = s_resolvedByAddress.find(phys);
            if (it == s_resolvedByAddress.end())
                return nullptr;
            ResolvedTexture* best = nullptr;
            for (ResolvedTexture* t : it->second)
            {
                bool sizeMatches = t->width == width && t->height == height;
                bool bestMatches = best && best->width == width && best->height == height;
                if (!best || (sizeMatches && !bestMatches) || (sizeMatches == bestMatches && t->lastResolve > best->lastResolve))
                    best = t;
            }
            return best;
        }

        // The host formats a resolve from source produces: color keeps the surface's
        // format (read back without sRGB conversion), depth becomes R32_FLOAT.
        void ResolvedFormats(const RenderTarget& source, DXGI_FORMAT& resource, DXGI_FORMAT& view)
        {
            if (source.depth)
            {
                resource = DXGI_FORMAT_R32_TYPELESS;
                view = DXGI_FORMAT_R32_FLOAT;
            }
            else
            {
                resource = source.resourceFormat;
                view = source.viewFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : source.viewFormat;
            }
        }

        ResolvedTexture* GetResolvedTexture(uint32_t phys, uint32_t width, uint32_t height, const RenderTarget& source)
        {
            DXGI_FORMAT resourceFormat, viewFormat;
            ResolvedFormats(source, resourceFormat, viewFormat);
            uint64_t key = uint64_t(phys) | uint64_t(width) << 29 | uint64_t(height) << 43 | uint64_t(resourceFormat) << 57;
            auto& slot = s_resolved[key];
            if (slot)
            {
                slot->lastResolve = ++s_resolveCount;
                slot->lastResolveFrame = slot->lastUsedFrame = FrameNumber();
                return slot.get();
            }

            auto t = std::make_unique<ResolvedTexture>();
            t->phys = phys;
            t->lastResolveFrame = t->lastUsedFrame = FrameNumber();
            t->width = width;
            t->height = height;
            t->resourceFormat = resourceFormat;
            t->viewFormat = viewFormat;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = t->resourceFormat;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            HRESULT hr = Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, t->state, nullptr,
                                                           IID_PPV_ARGS(&t->resource));
            if (FAILED(hr))
                Fatal("CreateCommittedResource(resolve target)", hr);
            t->srv = AllocateDescriptor(HeapKind::kView);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = t->viewFormat;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            Device()->CreateShaderResourceView(t->resource.Get(), &srv, CpuHandle(HeapKind::kView, t->srv));
            LOGI("render: resolve target at %08X: %ux%u", phys, width, height);
            t->lastResolve = ++s_resolveCount;
            s_resolvedByAddress[phys].push_back(t.get());
            slot = std::move(t);
            return slot.get();
        }

        // Bytes per pixel of a resolve destination format (a texture format).
        uint32_t ResolveBytesPerPixel(uint32_t format)
        {
            switch (format)
            {
            case 2: case 8: case 9: return 1;                                          // 8
            case 3: case 4: case 5: case 10: case 15: case 24: case 27: case 30: return 2; // 16-bit
            case 26: case 29: case 32: case 34: case 37: return 8;                     // 64-bit
            case 35: case 38: return 16;
            default: return 4;
            }
        }

        // With predicated tiling, Direct3D resolves each screen tile by moving the
        // destination base inside the full surface: a tiled surface stores 32x32
        // pixel tiles row-major, so base + n tiles is pixel (n % tilesPerRow * 32,
        // n / tilesPerRow * 32). Finds the surface such a base lands in.
        ResolvedTexture* FindResolveParent(uint32_t base, uint32_t width, uint32_t height, DXGI_FORMAT format,
                                           uint32_t bytesPerPixel, uint32_t& offsetX, uint32_t& offsetY)
        {
            if (bytesPerPixel < 4) // smaller tiles share pages; not handled
                return nullptr;
            // The address math depends on the pitch only; the moved destination may
            // describe a shorter surface (MCLA resolves its shadow atlas's lower
            // half with a 1280x640 destination inside the 1280x1280 one).
            // The parent's first tile was resolved earlier in the same frame. Games
            // reuse addresses across frames, so a surface from an older frame that
            // happens to contain the address is not a parent: taking it would send
            // a fresh first tile into a dead texture. Several surfaces of this frame
            // can still contain it; the tile belongs to the one resolved most recently.
            uint32_t tilesPerRow = (width + 31) / 32, tileBytes = 32 * 32 * bytesPerPixel;
            ResolvedTexture* best = nullptr;
            for (auto& [key, t] : s_resolved)
            {
                uint32_t surfaceBytes = tilesPerRow * ((t->height + 31) / 32) * tileBytes;
                if (t->lastResolveFrame != FrameNumber() || t->width != width || t->resourceFormat != format ||
                    t->phys >= base || base - t->phys >= surfaceBytes || (base - t->phys) % tileBytes)
                    continue;
                uint32_t tile = (base - t->phys) / tileBytes;
                if (tile / tilesPerRow * 32 >= t->height)
                    continue;
                if (!best || t->lastResolve > best->lastResolve)
                    best = t.get();
            }
            if (!best)
                return nullptr;
            uint32_t tile = (base - best->phys) / tileBytes;
            offsetX = tile % tilesPerRow * 32;
            offsetY = tile / tilesPerRow * 32;
            best->lastResolve = ++s_resolveCount;
            best->lastResolveFrame = best->lastUsedFrame = FrameNumber();
            return best;
        }

        // Frees resolve targets neither resolved into nor sampled for ~10 seconds,
        // so stale surfaces stop matching addresses that get reused.
        void EvictResolvedTextures()
        {
            constexpr uint64_t kUnusedFrames = 600;
            uint64_t frame = FrameNumber();
            if (frame % 300 != 0 || frame < kUnusedFrames)
                return;
            for (auto it = s_resolved.begin(); it != s_resolved.end();)
            {
                ResolvedTexture* t = it->second.get();
                if (t->lastUsedFrame + kUnusedFrames >= frame)
                {
                    ++it;
                    continue;
                }
                auto& list = s_resolvedByAddress[t->phys];
                list.erase(std::remove(list.begin(), list.end(), t), list.end());
                if (list.empty())
                    s_resolvedByAddress.erase(t->phys);
                FreeDescriptor(HeapKind::kView, t->srv);
                if (t->rtv)
                    FreeDescriptor(HeapKind::kRtv, t->rtv);
                DeferRelease(std::move(t->resource));
                it = s_resolved.erase(it);
            }
        }

        // Writes width x height pixels of dest at (destX, destY) from source's
        // samples starting at (sourceX, sourceY): the average of each pixel's
        // scaleX x scaleY samples, or its first sample (depth); convert: see kResolvePs.
        void ShaderResolve(RenderTarget& source, uint32_t sourceX, uint32_t sourceY, uint32_t scaleX, uint32_t scaleY,
                           ResolvedTexture& dest, uint32_t destX, uint32_t destY, uint32_t width, uint32_t height,
                           uint32_t convert)
        {
            ID3D12PipelineState* pipeline = ResolvePipeline(dest.viewFormat);
            if (!pipeline)
                return;
            ID3D12GraphicsCommandList* list = CommandList();
            if (!dest.rtv)
            {
                dest.rtv = AllocateDescriptor(HeapKind::kRtv);
                D3D12_RENDER_TARGET_VIEW_DESC rtv{};
                rtv.Format = dest.viewFormat;
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                Device()->CreateRenderTargetView(dest.resource.Get(), &rtv, CpuHandle(HeapKind::kRtv, dest.rtv));
            }
            uint32_t srv = RenderTargetSrv(source);
            Transition(list, source.resource.Get(), source.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(list, dest.resource.Get(), dest.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            uint32_t constants[8] = { sourceX, sourceY, scaleX, scaleY, destX, destY, source.depth ? 1u : 0u, convert };
            list->SetGraphicsRootSignature(s_resolveRootSignature.Get());
            ID3D12DescriptorHeap* heaps[] = { ViewHeap(), SamplerHeap() };
            list->SetDescriptorHeaps(2, heaps);
            list->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
            list->SetGraphicsRootDescriptorTable(1, GpuHandle(HeapKind::kView, srv));
            list->SetPipelineState(pipeline);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = CpuHandle(HeapKind::kRtv, dest.rtv);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            D3D12_VIEWPORT viewport{ float(destX), float(destY), float(width), float(height), 0.0f, 1.0f };
            D3D12_RECT scissor{ LONG(destX), LONG(destY), LONG(destX + width), LONG(destY + height) };
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
        }

        // ---- Pipelines ----

        struct PipelineKey
        {
            uint64_t vs, ps;
            uint32_t blend[4];
            uint32_t depthControl;
            uint32_t stencilMasks; // front read/write masks
            uint8_t rtFormats[4];
            uint8_t writeMask[4];
            uint8_t dsFormat;
            uint8_t numRenderTargets;
            uint8_t primitive; // xenos primitive type
            uint8_t cullMode;
            uint8_t frontCCW;
            uint8_t fillWire;
            uint8_t depthClip;
            uint8_t stripCut;  // 0 none, 1 16-bit, 2 32-bit
            uint8_t debugSolid; // MCLA_DEBUG_VS: solid magenta, no depth test or culling
            uint8_t streamOut;  // MCLA_SO_VS: positions captured through stream output
        };
        std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> s_pipelines;

        // Compiled pipelines persist in shader_cache/pipelines.bin (an
        // ID3D12PipelineLibrary), so pipelines seen in earlier runs load without
        // the driver compiling them again. The library is invalid after a driver
        // update; it is then started over.
        struct PipelineLibrary
        {
            ComPtr<ID3D12PipelineLibrary> library;
            std::vector<uint8_t> blob; // must outlive the library
            std::filesystem::path path;
            uint32_t unsaved = 0;
        } s_pipelineLibrary;

        void OpenPipelineLibrary()
        {
            ComPtr<ID3D12Device1> device1;
            if (FAILED(Device()->QueryInterface(IID_PPV_ARGS(&device1))) ||
                GetEnvironmentVariableA("MCLA_NO_PIPELINE_CACHE", nullptr, 0) > 0)
                return;
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            s_pipelineLibrary.path = std::filesystem::path(exe).parent_path() / "shader_cache" / "pipelines.bin";
            if (FILE* f = _wfopen(s_pipelineLibrary.path.c_str(), L"rb"))
            {
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (size > 0)
                {
                    s_pipelineLibrary.blob.resize(size_t(size));
                    if (fread(s_pipelineLibrary.blob.data(), 1, s_pipelineLibrary.blob.size(), f) != s_pipelineLibrary.blob.size())
                        s_pipelineLibrary.blob.clear();
                }
                fclose(f);
            }
            HRESULT hr = E_FAIL;
            if (!s_pipelineLibrary.blob.empty())
                hr = device1->CreatePipelineLibrary(s_pipelineLibrary.blob.data(), s_pipelineLibrary.blob.size(),
                                                    IID_PPV_ARGS(&s_pipelineLibrary.library));
            if (FAILED(hr))
            {
                if (!s_pipelineLibrary.blob.empty())
                    LOGI("render: pipeline cache does not match this driver (%08lX), starting a new one", hr);
                s_pipelineLibrary.blob.clear();
                if (FAILED(device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&s_pipelineLibrary.library))))
                    s_pipelineLibrary.library.Reset();
            }
            else
                LOGI("render: pipeline cache loaded (%zu KB)", s_pipelineLibrary.blob.size() >> 10);
        }

        // Called once per frame: writes the library after new pipelines were added.
        void SavePipelineLibrary(bool force)
        {
            auto& lib = s_pipelineLibrary;
            if (!lib.library || !lib.unsaved || (!force && lib.unsaved < 8 && FrameNumber() % 300 != 0))
                return;
            size_t size = lib.library->GetSerializedSize();
            std::vector<uint8_t> data(size);
            if (FAILED(lib.library->Serialize(data.data(), size)))
                return;
            std::filesystem::path temp = lib.path;
            temp += L".tmp";
            if (FILE* f = _wfopen(temp.c_str(), L"wb"))
            {
                bool ok = fwrite(data.data(), 1, size, f) == size;
                fclose(f);
                std::error_code ec;
                if (ok)
                    std::filesystem::rename(temp, lib.path, ec);
            }
            lib.unsaved = 0;
        }

        // MCLA_DEBUG_VS=<key>[,<key>...]: draws with these vertex shaders are drawn
        // solid magenta over everything, to see where their geometry lands.
        bool IsDebugVertexShader(uint64_t key)
        {
            static const std::vector<uint64_t> keys = [] {
                std::vector<uint64_t> list;
                char v[512];
                if (GetEnvironmentVariableA("MCLA_DEBUG_VS", v, sizeof(v)) > 0)
                    for (char* p = v; *p;)
                    {
                        char* end;
                        list.push_back(*p == '*' ? ~0ull : strtoull(p, &end, 16)); // '*': every shader
                        if (*p == '*')
                            end = p + 1;
                        p = *end ? end + 1 : end;
                    }
                return list;
            }();
            return std::find(keys.begin(), keys.end(), key) != keys.end() ||
                   std::find(keys.begin(), keys.end(), ~0ull) != keys.end();
        }

        // MCLA_SO_VS=<key> with MCLA_DRAW_LOG: the first draw of the logged frame with
        // that vertex shader records its host clip-space positions (stream output),
        // which are read back and logged.
        uint64_t StreamOutVertexShader()
        {
            static const uint64_t key = [] {
                char v[24];
                if (GetEnvironmentVariableA("MCLA_SO_VS", v, sizeof(v)) == 0)
                    return 0ull;
                return v[0] == '*' ? ~0ull : strtoull(v, nullptr, 16); // '*': every draw, bounds only
            }();
            return key;
        }

        struct StreamOutCapture
        {
            ComPtr<ID3D12Resource> buffer, readback; // positions, then the filled size at kDataBytes
            static constexpr uint64_t kDataBytes = 1 << 20;
            bool done = false;
            // The depth buffer (plane 0) as it was before the draw.
            ComPtr<ID3D12Resource> depthReadback;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthFootprint{};
            uint32_t depthWidth = 0, depthHeight = 0;
            float viewportWidth = 0, viewportHeight = 0;
        } s_streamOut;

        void CaptureDepthBeforeStreamOut(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, D3D12_RESOURCE_STATES& state)
        {
            D3D12_RESOURCE_DESC desc = depth->GetDesc();
            UINT64 total = 0;
            Device()->GetCopyableFootprints(&desc, 0, 1, 0, &s_streamOut.depthFootprint, nullptr, nullptr, &total);
            s_streamOut.depthWidth = uint32_t(desc.Width);
            s_streamOut.depthHeight = desc.Height;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total;
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&s_streamOut.depthReadback));
            D3D12_RESOURCE_STATES before = state;
            D3D12_RESOURCE_BARRIER b{};
            b.Transition.pResource = depth;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dst{ s_streamOut.depthReadback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint = s_streamOut.depthFootprint;
            D3D12_TEXTURE_COPY_LOCATION src{ depth, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            list->ResourceBarrier(1, &b);
            LOGI("draw:   depth target %ux%u, footprint format %d", s_streamOut.depthWidth, s_streamOut.depthHeight,
                 int(s_streamOut.depthFootprint.Footprint.Format));
        }

        // Logs the stored depth under a host clip-space position (D24: low 24 bits unorm, D32: float).
        void LogDepthUnder(const float* p)
        {
            if (!s_streamOut.depthReadback || p[3] == 0.0f)
                return;
            float x = (p[0] / p[3] + 1.0f) * 0.5f * s_streamOut.viewportWidth;
            float y = (1.0f - p[1] / p[3]) * 0.5f * s_streamOut.viewportHeight;
            if (x < 0 || y < 0 || x >= float(s_streamOut.depthWidth) || y >= float(s_streamOut.depthHeight))
            {
                LOGI("draw:   vertex at %.1f,%.1f is off the target", x, y);
                return;
            }
            uint8_t* data = nullptr;
            D3D12_RANGE range{ 0, SIZE_T(-1) };
            if (FAILED(s_streamOut.depthReadback->Map(0, nullptr, reinterpret_cast<void**>(&data))))
                return;
            const auto& f = s_streamOut.depthFootprint;
            uint32_t raw;
            memcpy(&raw, data + f.Offset + size_t(uint32_t(y)) * f.Footprint.RowPitch + size_t(uint32_t(x)) * 4, 4);
            float asFloat;
            memcpy(&asFloat, &raw, 4);
            LOGI("draw:   at sample %u,%u: vertex depth %g, stored %08X (unorm24 %g, float %g)", uint32_t(x), uint32_t(y),
                 p[2] / p[3], raw, double(raw & 0xFFFFFF) / 16777215.0, asFloat);
            D3D12_RANGE none{ 0, 0 };
            s_streamOut.depthReadback->Unmap(0, &none);
            (void)range;
        }

        void BeginStreamOut(ID3D12GraphicsCommandList* list)
        {
            auto makeBuffer = [](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out) {
                if (out)
                    return; // reused (stream-out buffers are left in COPY_DEST below)
                D3D12_HEAP_PROPERTIES heap{ type };
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                desc.Width = StreamOutCapture::kDataBytes + 256;
                desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
                desc.SampleDesc.Count = 1;
                desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out));
            };
            makeBuffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, s_streamOut.buffer);
            makeBuffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, s_streamOut.readback);
            UploadSpace zero = AllocateUpload(8, 8);
            memset(zero.cpu, 0, 8);
            list->CopyBufferRegion(s_streamOut.buffer.Get(), StreamOutCapture::kDataBytes, zero.resource, zero.offset, 8);
            D3D12_RESOURCE_BARRIER b{};
            b.Transition.pResource = s_streamOut.buffer.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_STREAM_OUT;
            list->ResourceBarrier(1, &b);
            D3D12_STREAM_OUTPUT_BUFFER_VIEW view{ s_streamOut.buffer->GetGPUVirtualAddress(), StreamOutCapture::kDataBytes,
                                                  s_streamOut.buffer->GetGPUVirtualAddress() + StreamOutCapture::kDataBytes };
            list->SOSetTargets(0, 1, &view);
        }

        void EndStreamOut(ID3D12GraphicsCommandList* list)
        {
            list->SOSetTargets(0, 0, nullptr);
            D3D12_RESOURCE_BARRIER b{};
            b.Transition.pResource = s_streamOut.buffer.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_STREAM_OUT;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &b);
            list->CopyResource(s_streamOut.readback.Get(), s_streamOut.buffer.Get());
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            list->ResourceBarrier(1, &b);
            Flush();
            WaitIdle();
            bool all = StreamOutVertexShader() == ~0ull;
            uint8_t* data = nullptr;
            D3D12_RANGE range{ 0, size_t(StreamOutCapture::kDataBytes + 8) };
            if (FAILED(s_streamOut.readback->Map(0, &range, reinterpret_cast<void**>(&data))))
                return;
            uint64_t filled;
            memcpy(&filled, data + StreamOutCapture::kDataBytes, 8);
            const float* p = reinterpret_cast<const float*>(data);
            if (!all)
            {
                LOGI("draw:   stream output: %llu bytes", (unsigned long long)filled);
                for (uint32_t i = 0; i < 12 && i * 16 < filled; i++)
                    LOGI("draw:   position %u: %g %g %g %g", i, p[i * 4], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3]);
                for (uint32_t i = 0; i < 4 && i * 16 * 97 < filled; i++)
                    LogDepthUnder(p + i * 97 * 4);
            }
            // Screen bounds of everything drawn (in viewport pixels, before MSAA scaling).
            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            uint32_t behind = 0;
            for (uint64_t i = 0; i * 16 < filled; i++)
            {
                const float* v = p + i * 4;
                if (v[3] <= 0.0f)
                {
                    behind++;
                    continue;
                }
                float x = (v[0] / v[3] + 1.0f) * 0.5f * s_streamOut.viewportWidth;
                float y = (1.0f - v[1] / v[3]) * 0.5f * s_streamOut.viewportHeight;
                minX = std::min(minX, x), maxX = std::max(maxX, x), minY = std::min(minY, y), maxY = std::max(maxY, y);
            }
            LOGI("draw:   bounds %.0f,%.0f - %.0f,%.0f (samples), %u vertices behind the camera", minX, minY, maxX, maxY, behind);
            D3D12_RANGE none{ 0, 0 };
            s_streamOut.readback->Unmap(0, &none);
            s_streamOut.done = !all;
        }

        D3D12_BLEND BlendFactor(uint32_t f, bool alpha)
        {
            switch (f)
            {
            case 0: return D3D12_BLEND_ZERO;
            case 1: return D3D12_BLEND_ONE;
            case 4: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
            case 5: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
            case 6: return D3D12_BLEND_SRC_ALPHA;
            case 7: return D3D12_BLEND_INV_SRC_ALPHA;
            case 8: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
            case 9: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
            case 10: return D3D12_BLEND_DEST_ALPHA;
            case 11: return D3D12_BLEND_INV_DEST_ALPHA;
            case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;
            case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
            case 16: return alpha ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA_SAT;
            default: return D3D12_BLEND_ONE;
            }
        }

        D3D12_BLEND_OP BlendOp(uint32_t op)
        {
            switch (op)
            {
            case 1: return D3D12_BLEND_OP_SUBTRACT;
            case 2: return D3D12_BLEND_OP_MIN;
            case 3: return D3D12_BLEND_OP_MAX;
            case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
            default: return D3D12_BLEND_OP_ADD;
            }
        }

        D3D12_COMPARISON_FUNC Compare(uint32_t f) { return D3D12_COMPARISON_FUNC(D3D12_COMPARISON_FUNC_NEVER + (f & 7)); }
        D3D12_STENCIL_OP StencilOp(uint32_t op) { return D3D12_STENCIL_OP(D3D12_STENCIL_OP_KEEP + (op & 7)); }

        ID3D12PipelineState* GetPipeline(const PipelineKey& key, const HostShader& vs, const HostShader& ps)
        {
            uint64_t hash = XXH3_64bits(&key, sizeof(key));
            auto& slot = s_pipelines[hash];
            if (slot)
                return slot.Get();

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = s_rootSignature.Get();
            pd.VS = vs.Bytecode();
            pd.PS = ps.Bytecode();
            if (key.primitive == 8)
                pd.GS = { s_rectListGs.data(), s_rectListGs.size() };
            else if (key.primitive == 13)
                pd.GS = { s_quadListGs.data(), s_quadListGs.size() };

            pd.BlendState.IndependentBlendEnable = TRUE;
            for (uint32_t i = 0; i < key.numRenderTargets; i++)
            {
                auto& rt = pd.BlendState.RenderTarget[i];
                uint32_t b = key.blend[i];
                uint32_t srcC = b & 0x1F, opC = (b >> 5) & 7, dstC = (b >> 8) & 0x1F;
                uint32_t srcA = (b >> 16) & 0x1F, opA = (b >> 21) & 7, dstA = (b >> 24) & 0x1F;
                bool passthrough = srcC == 1 && dstC == 0 && opC == 0 && srcA == 1 && dstA == 0 && opA == 0;
                rt.BlendEnable = !passthrough;
                rt.SrcBlend = BlendFactor(srcC, false);
                rt.DestBlend = BlendFactor(dstC, false);
                rt.BlendOp = BlendOp(opC);
                rt.SrcBlendAlpha = BlendFactor(srcA, true);
                rt.DestBlendAlpha = BlendFactor(dstA, true);
                rt.BlendOpAlpha = BlendOp(opA);
                rt.RenderTargetWriteMask = key.writeMask[i];
                pd.RTVFormats[i] = DXGI_FORMAT(key.rtFormats[i]);
            }
            pd.NumRenderTargets = key.numRenderTargets;
            pd.SampleMask = UINT_MAX;

            pd.RasterizerState.FillMode = key.fillWire ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE(key.cullMode);
            pd.RasterizerState.FrontCounterClockwise = key.frontCCW;
            pd.RasterizerState.DepthClipEnable = key.depthClip;

            uint32_t dc = key.depthControl;
            if (key.dsFormat)
            {
                pd.DSVFormat = DXGI_FORMAT(key.dsFormat);
                pd.DepthStencilState.DepthEnable = (dc >> 1) & 1;
                pd.DepthStencilState.DepthWriteMask = ((dc >> 2) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
                pd.DepthStencilState.DepthFunc = Compare(dc >> 4);
                pd.DepthStencilState.StencilEnable = dc & 1;
                pd.DepthStencilState.StencilReadMask = uint8_t(key.stencilMasks);
                pd.DepthStencilState.StencilWriteMask = uint8_t(key.stencilMasks >> 8);
                auto& front = pd.DepthStencilState.FrontFace;
                front.StencilFunc = Compare(dc >> 8);
                front.StencilFailOp = StencilOp(dc >> 11);
                front.StencilPassOp = StencilOp(dc >> 14);
                front.StencilDepthFailOp = StencilOp(dc >> 17);
                auto& back = pd.DepthStencilState.BackFace;
                if ((dc >> 7) & 1)
                {
                    back.StencilFunc = Compare(dc >> 20);
                    back.StencilFailOp = StencilOp(dc >> 23);
                    back.StencilPassOp = StencilOp(dc >> 26);
                    back.StencilDepthFailOp = StencilOp(dc >> 29);
                }
                else
                {
                    back = front;
                }
            }

            switch (key.primitive)
            {
            case 1: pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; break;
            case 2: case 3: case 13: pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
            default: pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; break;
            }
            pd.IBStripCutValue = key.stripCut == 1   ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                                 : key.stripCut == 2 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                                     : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
            pd.SampleDesc.Count = 1;
            static const D3D12_SO_DECLARATION_ENTRY soEntry{ 0, "SV_POSITION", 0, 0, 4, 0 };
            static const UINT soStride = 16;
            if (key.streamOut)
                pd.StreamOutput = { &soEntry, 1, &soStride, 1, 0 };
            // MCLA_DEBUG_NO_STENCIL: the debug draws keep all their state but write no stencil.
            static const bool noStencil = [] {
                char v[4];
                return GetEnvironmentVariableA("MCLA_DEBUG_NO_STENCIL", v, sizeof(v)) > 0;
            }();
            if (key.debugSolid && noStencil)
            {
                pd.DepthStencilState.StencilEnable = FALSE;
            }
            else if (key.debugSolid)
            {
                // MCLA_DEBUG_KEEP_PS: keep the real pixel shader (isolates the depth test).
                static const bool keepPs = [] {
                    char v[4];
                    return GetEnvironmentVariableA("MCLA_DEBUG_KEEP_PS", v, sizeof(v)) > 0;
                }();
                if (!keepPs)
                    pd.PS = { s_debugPs.data(), s_debugPs.size() };
                // Always pass but write depth, so later geometry behind it doesn't paint over it.
                pd.DepthStencilState = {};
                pd.DepthStencilState.DepthEnable = key.dsFormat != 0;
                pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
                pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.BlendState.RenderTarget[0].BlendEnable = FALSE;
                pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            }

            LARGE_INTEGER psoStart, psoEnd;
            QueryPerformanceCounter(&psoStart);
            wchar_t name[20];
            swprintf(name, 20, L"%016llX", (unsigned long long)hash);
            HRESULT hr = E_FAIL;
            if (s_pipelineLibrary.library)
                hr = s_pipelineLibrary.library->LoadGraphicsPipeline(name, &pd, IID_PPV_ARGS(&slot));
            if (FAILED(hr))
            {
                hr = Device()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&slot));
                if (SUCCEEDED(hr) && s_pipelineLibrary.library &&
                    SUCCEEDED(s_pipelineLibrary.library->StorePipeline(name, slot.Get())))
                    s_pipelineLibrary.unsaved++;
            }
            QueryPerformanceCounter(&psoEnd);
            s_framePsoTicks += psoEnd.QuadPart - psoStart.QuadPart;
            s_framePsos++;
            if (FAILED(hr))
            {
                static int logged = 0;
                if (logged++ < 10)
                    LOGI("render: pipeline creation failed (%08lX), VS %016llX PS %016llX prim %u, %u RTs (%u %u %u %u, "
                         "masks %X %X %X %X, blend %08X %08X %08X %08X), DS %u ctl %08X, PS outputs %X depth %u",
                         hr, (unsigned long long)key.vs, (unsigned long long)key.ps, key.primitive, key.numRenderTargets,
                         key.rtFormats[0], key.rtFormats[1], key.rtFormats[2], key.rtFormats[3], key.writeMask[0],
                         key.writeMask[1], key.writeMask[2], key.writeMask[3], key.blend[0], key.blend[1], key.blend[2],
                         key.blend[3], key.dsFormat, key.depthControl, ps.colorOutputs, ps.writesDepth);
                return nullptr;
            }
            s_stats.pipelines++;
            return slot.Get();
        }

        // ---- Per-draw state ----

        struct Scissor
        {
            int32_t left, top, right, bottom;
        };

        Scissor GetScissor()
        {
            uint32_t tl = Reg(PA_SC_WINDOW_SCISSOR_TL), br = Reg(PA_SC_WINDOW_SCISSOR_BR);
            int32_t left = tl & 0x3FFF, top = (tl >> 16) & 0x3FFF, right = br & 0x3FFF, bottom = (br >> 16) & 0x3FFF;
            if (!(tl >> 31)) // window offset not disabled
            {
                uint32_t wo = Reg(PA_SC_WINDOW_OFFSET);
                int32_t ox = SignExtend(wo, 15), oy = SignExtend(wo >> 16, 15);
                left += ox;
                right += ox;
                top += oy;
                bottom += oy;
            }
            uint32_t stl = Reg(PA_SC_SCREEN_SCISSOR_TL), sbr = Reg(PA_SC_SCREEN_SCISSOR_BR);
            left = std::max(left, SignExtend(stl, 15));
            top = std::max(top, SignExtend(stl >> 16, 15));
            right = std::min(right, SignExtend(sbr, 15));
            bottom = std::min(bottom, SignExtend(sbr >> 16, 15));
            return { std::max(left, 0), std::max(top, 0), std::max(right, 0), std::max(bottom, 0) };
        }

        // ---- Pixel history (debugging) ----
        // MCLA_PICK=<x>,<y> with MCLA_DRAW_LOG: after every draw of the logged frame,
        // render target 0's sample at (x, y) is copied out (sample coordinates: a 2x
        // MSAA surface stores a pixel as 1x2 samples, 4x as 2x2). At the end of the
        // frame every draw that changed it is logged with the value before and after.
        struct PickRecord
        {
            ID3D12Resource* resource;
            DXGI_FORMAT format;
            uint64_t vs, ps;
            uint32_t draw, flags, writeMask;
            bool before; // copied before the draw (else after it)
        };
        uint32_t s_lastDrawFlags = 0, s_lastDrawWriteMask = 0;
        std::vector<PickRecord> s_picks;
        ComPtr<ID3D12Resource> s_pickReadback;
        uint32_t s_frameDrawIndex = 0;
        constexpr uint32_t kPickSlotBytes = 512, kMaxPicks = 8192;

        bool PickCoordinates(uint32_t& x, uint32_t& y)
        {
            static const int64_t packed = [] {
                char v[32];
                unsigned px, py;
                if (GetEnvironmentVariableA("MCLA_PICK", v, sizeof(v)) > 0 && sscanf(v, "%u,%u", &px, &py) == 2)
                    return int64_t(px) << 32 | py;
                return int64_t(-1);
            }();
            if (packed < 0)
                return false;
            x = uint32_t(packed >> 32);
            y = uint32_t(packed);
            return true;
        }

        void RecordPick(ID3D12GraphicsCommandList* list, RenderTarget* rt, uint64_t vs, uint64_t ps, bool before)
        {
            uint32_t x, y;
            if (!rt || !PickCoordinates(x, y) || x >= rt->width || y >= rt->height || s_picks.size() >= kMaxPicks)
                return;
            if (!s_pickReadback)
            {
                D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                desc.Width = uint64_t(kMaxPicks) * kPickSlotBytes;
                desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
                desc.SampleDesc.Count = 1;
                desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (FAILED(Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                             nullptr, IID_PPV_ARGS(&s_pickReadback))))
                    return;
            }
            D3D12_RESOURCE_STATES state = rt->state;
            Transition(list, rt->resource.Get(), rt->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = s_pickReadback.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = uint64_t(s_picks.size()) * kPickSlotBytes;
            dst.PlacedFootprint.Footprint = { rt->resourceFormat, 1, 1, 1, 256 };
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = rt->resource.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_BOX box{ x, y, 0, x + 1, y + 1, 1 };
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
            Transition(list, rt->resource.Get(), rt->state, state);
            s_picks.push_back(
                { rt->resource.Get(), rt->resourceFormat, vs, ps, s_frameDrawIndex, s_lastDrawFlags, s_lastDrawWriteMask, before });
        }

        void FormatPickValue(DXGI_FORMAT format, const uint8_t* p, char* out, size_t size)
        {
            switch (format)
            {
            case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            {
                uint16_t h[4];
                memcpy(h, p, 8);
                auto f = [](uint16_t v) {
                    uint32_t sign = (v >> 15) & 1, exp = (v >> 10) & 0x1F, mant = v & 0x3FF;
                    float r = exp == 0 ? std::ldexp(float(mant), -24) : exp == 31 ? (mant ? NAN : INFINITY)
                                                                                    : std::ldexp(float(mant | 0x400), int(exp) - 25);
                    return sign ? -r : r;
                };
                snprintf(out, size, "%.4g %.4g %.4g %.4g", f(h[0]), f(h[1]), f(h[2]), f(h[3]));
                break;
            }
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                snprintf(out, size, "%u %u %u %u", p[0], p[1], p[2], p[3]);
                break;
            default:
            {
                uint32_t v[2];
                memcpy(v, p, 8);
                snprintf(out, size, "%08X %08X", v[0], v[1]);
                break;
            }
            }
        }

        // After the logged frame was submitted: waits for it and logs the draws that
        // changed the picked sample.
        void ReportPicks()
        {
            if (s_picks.empty())
                return;
            WaitIdle();
            uint8_t* data = nullptr;
            D3D12_RANGE range{ 0, s_picks.size() * kPickSlotBytes };
            if (SUCCEEDED(s_pickReadback->Map(0, &range, reinterpret_cast<void**>(&data))))
            {
                std::unordered_map<ID3D12Resource*, const uint8_t*> last;
                for (size_t i = 0; i < s_picks.size(); i++)
                {
                    const PickRecord& r = s_picks[i];
                    const uint8_t* now = data + i * kPickSlotBytes;
                    auto it = last.find(r.resource);
                    if (it == last.end() || memcmp(it->second, now, 8) != 0)
                    {
                        char before[96] = "(first use of this target)", after[96];
                        if (it != last.end())
                            FormatPickValue(r.format, it->second, before, sizeof(before));
                        FormatPickValue(r.format, now, after, sizeof(after));
                        if (r.before)
                            LOGI("pick: changed outside draws, before draw %u (target %p): %s -> %s", r.draw, (void*)r.resource,
                                 before, after);
                        else
                            LOGI("pick: draw %u (vs %016llX ps %016llX, mask %X flags %X) target %p: %s -> %s", r.draw,
                                 (unsigned long long)r.vs, (unsigned long long)r.ps, r.writeMask, r.flags, (void*)r.resource,
                                 before, after);
                    }
                    last[r.resource] = now;
                }
                D3D12_RANGE none{ 0, 0 };
                s_pickReadback->Unmap(0, &none);
            }
            s_picks.clear();
        }

        // dimension: the shader's tfetch dimension (0 1D, 1 2D, 2 3D/stacked, 3 cube).
        uint32_t TextureIndexForSlot(const uint32_t fetch[6], uint32_t dimension)
        {
            if (dimension == 1 || dimension == 0) // 1D/2D: render-to-texture results first
            {
                uint32_t width = (fetch[2] & 0x1FFF) + 1, height = ((fetch[2] >> 13) & 0x1FFF) + 1;
                if (ResolvedTexture* t = FindResolved(fetch[1] & 0x1FFFF000, width, height))
                {
                    t->lastUsedFrame = FrameNumber();
                    Transition(CommandList(), t->resource.Get(), t->state,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    return t->srv;
                }
            }
            if (uint32_t descriptor = GetTextureDescriptor(fetch, dimension))
                return descriptor;
            return dimension <= 1 ? 0 : (dimension == 2 ? 1 : 2); // placeholders
        }

        void DoResolve();
    }

    bool InitRenderer()
    {
        InitDescriptors();
        InitSharedMemory();
        CreateRootSignatures();
        if (!InitShaderCache() || !CreateBuiltinShaders())
            return false;
        CreatePlaceholders();
        OpenPipelineLibrary();
        InitTextureCache();
        LOGI("render: renderer ready");
        return true;
    }

    bool LoggingFrame() { return LogDraws(); }

    void Draw(const DrawPacket& packet)
    {
        ScopeTimer timer(s_stats.drawTicks);
        SectionClock clock;
        s_stats.draws++;
        uint32_t edramMode = Reg(RB_MODECONTROL) & 7;
        if (edramMode == 6)
        {
            DoResolve();
            clock.Lap(kSecResolve);
            return;
        }
        if (edramMode != 4 && edramMode != 5)
            return;

        uint32_t prim = packet.initiator & 0x3F;
        uint32_t source = (packet.initiator >> 6) & 3;
        uint32_t count = packet.initiator >> 16;
        bool indexed = source == 0;
        bool index32 = (packet.initiator >> 11) & 1;
        if (!count)
            return;
        // Fans, polygons and line loops have no host topology: they are drawn as
        // triangle lists / line strips through generated indices.
        uint32_t hostPrim = prim;
        switch (prim)
        {
        case 1: case 2: case 3: case 4: case 6: case 8: case 13:
            break;
        case 5: case 15: // triangle fan, polygon
            if (count < 3)
                return;
            hostPrim = 4;
            break;
        case 12: // line loop
            if (count < 2)
                return;
            hostPrim = 3;
            break;
        default: // 2D copy primitives, quad strips, triangles with flags: not yet
            s_stats.skippedPrimitive++;
            if (LogDraws())
                LOGI("draw: primitive %u (%u vertices) skipped", prim, count);
            return;
        }
        bool generateIndices = hostPrim != prim;

        if (!packet.vs || !packet.ps)
        {
            s_stats.skippedShaderUnknown++;
            if (LogDraws())
                LOGI("draw: prim %u, %u verts, mode %u skipped: %s%s unknown (vs code %08X, %u dwords; ps code %08X, %u dwords)",
                     prim, count, edramMode, packet.vs ? "" : "vs ", packet.ps ? "" : "ps", packet.vsCode, packet.vsDwords,
                     packet.psCode, packet.psDwords);
            return;
        }
        HostShader* vs = GetHostShader(packet.vs, packet.vsCode, packet.vsDwords, packet.ps);
        HostShader* ps = GetHostShader(packet.ps, packet.psCode, packet.psDwords, packet.vs);
        if (!vs || !ps)
        {
            s_stats.skippedShaderUnknown++;
            return;
        }
        if (LogDraws())
            LOGI("draw: prim %u, %u %s, mode %u, vs %016llX (%u) ps %016llX (%u) containers %016llX %016llX", prim, count,
                 indexed ? (index32 ? "idx32" : "idx16") : "auto", edramMode, (unsigned long long)vs->key, vs->state.load(),
                 (unsigned long long)ps->key, ps->state.load(), (unsigned long long)packet.vs->hash,
                 (unsigned long long)packet.ps->hash);
        uint32_t vsState = vs->state.load(std::memory_order_acquire), psState = ps->state.load(std::memory_order_acquire);
        if (vsState == HostShader::kFailed || psState == HostShader::kFailed)
        {
            s_stats.skippedShaderFailed++;
            return;
        }
        if (vsState != HostShader::kReady || psState != HostShader::kReady)
        {
            s_stats.skippedShaderPending++;
            return;
        }
        clock.Lap(kSecShaders);

        ID3D12GraphicsCommandList* list = CommandList();

        // Render targets.
        uint32_t surfaceInfo = Reg(RB_SURFACE_INFO);
        uint32_t pitch = surfaceInfo & 0x3FFF, msaa = (surfaceInfo >> 16) & 3;
        uint32_t colorMask = Reg(RB_COLOR_MASK);
        PipelineKey key;
        memset(&key, 0, sizeof(key));
        key.vs = vs->key;
        key.ps = ps->key;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4];
        RenderTarget* colorTargets[4] = {};
        RenderTarget* sizeSource = nullptr;
        uint32_t numRT = 0;
        if (edramMode == 4)
        {
            for (uint32_t i = 0; i < 4; i++)
            {
                uint32_t mask = (colorMask >> (i * 4)) & 0xF;
                if (!mask || !(ps->colorOutputs & (1u << i)))
                    continue;
                uint32_t info = Reg(kColorInfo[i]);
                RenderTarget* rt = GetRenderTarget(info & 0xFFF, pitch, msaa, (info >> 16) & 0xF, false);
                if (!rt)
                    continue;
                DiscardIfOverwritten(CommandList(), *rt);
                colorTargets[i] = rt;
                numRT = i + 1;
                key.writeMask[i] = uint8_t(mask);
                key.blend[i] = Reg(kBlendControl[i]);
                sizeSource = sizeSource ? sizeSource : rt;
            }
            for (uint32_t i = 0; i < numRT; i++)
            {
                if (colorTargets[i])
                {
                    Transition(list, colorTargets[i]->resource.Get(), colorTargets[i]->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    rtvs[i] = CpuHandle(HeapKind::kRtv, colorTargets[i]->view);
                    key.rtFormats[i] = uint8_t(colorTargets[i]->viewFormat);
                }
                else
                {
                    rtvs[i] = CpuHandle(HeapKind::kRtv, s_nullRtv);
                    key.rtFormats[i] = uint8_t(DXGI_FORMAT_R8G8B8A8_UNORM);
                    key.blend[i] = 0x00010001;
                }
            }
        }
        key.numRenderTargets = uint8_t(numRT);

        uint32_t depthControl = Reg(RB_DEPTHCONTROL);
        RenderTarget* depthTarget = nullptr;
        if (depthControl & 0x7) // stencil, z test or z write
        {
            uint32_t info = Reg(RB_DEPTH_INFO);
            depthTarget = GetRenderTarget(info & 0xFFF, pitch, msaa, (info >> 16) & 1, true);
            if (depthTarget)
            {
                Transition(list, depthTarget->resource.Get(), depthTarget->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                key.dsFormat = uint8_t(depthTarget->viewFormat);
                key.depthControl = depthControl;
                uint32_t refMask = Reg(RB_STENCILREFMASK);
                key.stencilMasks = ((refMask >> 8) & 0xFF) | (((refMask >> 16) & 0xFF) << 8);
                sizeSource = sizeSource ? sizeSource : depthTarget;
            }
        }
        if (!sizeSource)
            return; // nothing to draw into

        // Rasterizer.
        uint32_t mode = Reg(PA_SU_SC_MODE_CNTL);
        bool cullFront = mode & 1, cullBack = (mode >> 1) & 1;
        if (cullFront && cullBack && hostPrim >= 4)
            return;
        key.cullMode = uint8_t(prim == 8 || prim == 13 ? D3D12_CULL_MODE_NONE
                                : cullFront          ? D3D12_CULL_MODE_FRONT
                                : cullBack           ? D3D12_CULL_MODE_BACK
                                                     : D3D12_CULL_MODE_NONE);
        key.frontCCW = ((mode >> 2) & 1) == 0;
        uint32_t polyMode = (mode >> 3) & 3;
        key.fillWire = polyMode == 1 && ((mode >> 5) & 7) == 1;
        key.depthClip = !((Reg(PA_CL_CLIP_CNTL) >> 16) & 1);
        key.primitive = uint8_t(hostPrim);
        if ((prim == 3 || prim == 6) && indexed && ((mode >> 21) & 1))
            key.stripCut = index32 ? 2 : 1;
        key.debugSolid = numRT && IsDebugVertexShader(vs->key);
        // MCLA_SO_COUNT=<n>: only a draw of that many vertices/indices.
        static const uint32_t soCount = [] {
            char v[16];
            return GetEnvironmentVariableA("MCLA_SO_COUNT", v, sizeof(v)) > 0 ? uint32_t(atoi(v)) : 0u;
        }();
        uint64_t soKey = StreamOutVertexShader();
        bool streamOut = soKey && LogDraws() && !s_streamOut.done && (soKey == ~0ull || vs->key == soKey) &&
                         hostPrim == 4 && !generateIndices && (!soCount || count == soCount);
        key.streamOut = streamOut;
        clock.Lap(kSecTargets);

        ID3D12PipelineState* pipeline = GetPipeline(key, *vs, *ps);
        clock.Lap(kSecPipeline);
        if (!pipeline)
            return;

        // Guest memory the draw reads.
        for (uint32_t w = 0; w < 3; w++)
        {
            for (uint32_t bits = vs->vertexFetchSlots[w]; bits; bits &= bits - 1)
            {
                uint32_t vfc = w * 32 + uint32_t(__builtin_ctz(bits));
                uint32_t d0 = Reg(SHADER_CONSTANT_FETCH_00_0 + vfc * 2), d1 = Reg(SHADER_CONSTANT_FETCH_00_0 + vfc * 2 + 1);
                if ((d0 & 3) == 3)
                    UseSharedMemory(d0 & 0x1FFFFFFC, ((d1 >> 2) & 0xFFFFFF) * 4);
            }
        }
        uint32_t indexBytes = 0;
        if (indexed && !generateIndices)
        {
            indexBytes = count * (index32 ? 4 : 2);
            UseSharedMemory(packet.indexBase & 0x1FFFFFFF, indexBytes);
        }
        if (CommitSharedMemoryUploads(list))
            s_stats.mirrorCommits++;
        clock.Lap(kSecMirror);

        // Constants.
        D3D12_GPU_VIRTUAL_ADDRESS vsConstants = UploadConstantBank(s_vsBank, SHADER_CONSTANT_000_X);
        D3D12_GPU_VIRTUAL_ADDRESS psConstants = UploadConstantBank(s_psBank, SHADER_CONSTANT_256_X);
        UploadSpace systemSpace = AllocateUpload(sizeof(xenos::SystemConstants));
        auto* sys = reinterpret_cast<xenos::SystemConstants*>(systemSpace.cpu);
        // The upload heap is write-combined: never read it back. The texture lookups
        // below use a local copy of the fetch constants.
        uint32_t fetchConstants[192];
        ReadRegisters(SHADER_CONSTANT_FETCH_00_0, 192, fetchConstants);
        memcpy(sys->fetch, fetchConstants, sizeof(fetchConstants));
        ReadRegisters(SHADER_CONSTANT_BOOL_000_031, 8, sys->bools);
        ReadRegisters(SHADER_CONSTANT_LOOP_00, 32, sys->loops);
        uint32_t textureIndex[32] = {}, samplerIndex[32] = {};
        for (uint32_t used = vs->textureSlots | ps->textureSlots; used; used &= used - 1)
        {
            uint32_t slot = uint32_t(__builtin_ctz(used));
            uint32_t dim = (vs->textureSlots & (1u << slot)) ? vs->textureDims[slot] : ps->textureDims[slot];
            const uint32_t* fetch = fetchConstants + slot * 6;
            textureIndex[slot] = TextureIndexForSlot(fetch, dim);
            samplerIndex[slot] = GetSamplerDescriptor(fetch);
        }
        memcpy(sys->textureIndex, textureIndex, sizeof(textureIndex));
        memcpy(sys->samplerIndex, samplerIndex, sizeof(samplerIndex));
        clock.Lap(kSecBindings);
        struct IssueLap
        {
            SectionClock& clock;
            ~IssueLap() { clock.Lap(kSecIssue); }
        } issueLap{ clock };

        // Viewport transform: guest clip space -> guest screen -> host clip space
        // over the whole render target.
        uint32_t vte = Reg(PA_CL_VTE_CNTL);
        float sx = (vte & 1) ? RegF(PA_CL_VPORT_XSCALE) : 1.0f, ox = (vte & 2) ? RegF(PA_CL_VPORT_XOFFSET) : 0.0f;
        float sy = (vte & 4) ? RegF(PA_CL_VPORT_YSCALE) : 1.0f, oy = (vte & 8) ? RegF(PA_CL_VPORT_YOFFSET) : 0.0f;
        float sz = (vte & 16) ? RegF(PA_CL_VPORT_ZSCALE) : 1.0f, oz = (vte & 32) ? RegF(PA_CL_VPORT_ZOFFSET) : 0.0f;
        if ((mode >> 16) & 1) // vtx_window_offset_enable
        {
            uint32_t wo = Reg(PA_SC_WINDOW_OFFSET);
            ox += float(SignExtend(wo, 15));
            oy += float(SignExtend(wo >> 16, 15));
        }
        if ((Reg(PA_SU_VTX_CNTL) & 1) == 0) // Direct3D 9 pixel centers
        {
            ox += 0.5f;
            oy += 0.5f;
        }
        // Surfaces are sample grids: a multisampled draw covers scale x scale samples per pixel.
        float msaaX = float(MsaaScaleX(msaa)), msaaY = float(MsaaScaleY(msaa));
        sys->pixelScale[0] = msaaX;
        sys->pixelScale[1] = msaaY;
        sys->pixelScale[2] = sys->pixelScale[3] = 1.0f;
        float w = float(sizeSource->width), h = float(sizeSource->height);
        sys->ndcScale[0] = sx * msaaX * 2.0f / w;
        sys->ndcScale[1] = -sy * msaaY * 2.0f / h;
        sys->ndcScale[2] = sz;
        sys->ndcOffset[0] = ox * msaaX * 2.0f / w - 1.0f;
        sys->ndcOffset[1] = 1.0f - oy * msaaY * 2.0f / h;
        sys->ndcOffset[2] = oz;
        // VTX_W0_FMT (bit 10) set: W is the real W and the hardware takes its reciprocal;
        // clear: the shader already wrote 1/W.
        uint32_t flags = ((vte >> 8) & 1 ? xenos::kFlagXYNotDividedByW : 0) | ((vte >> 9) & 1 ? xenos::kFlagZNotDividedByW : 0) |
                         ((vte >> 10) & 1 ? 0 : xenos::kFlagWIsReciprocal);
        for (uint32_t i = 0; i < 4; i++)
        {
            uint32_t info = Reg(kColorInfo[i]);
            uint32_t guestFormat = (info >> 16) & 0xF;
            int32_t expBias = int32_t((info >> 20) & 0x3F);
            if (expBias & 0x20)
                expBias -= 64;
            sys->colorExpBias[i] = i < numRT && colorTargets[i] ? std::ldexp(1.0f, expBias) : 1.0f;
            if (i < numRT && colorTargets[i] && (guestFormat == 3 || guestFormat == 12))
            {
                flags |= xenos::kFlagColor7e3 << i;
                if (key.blend[i] == 0x00010001)
                    flags |= xenos::kFlagColor7e3Unblended << i;
            }
        }
        sys->flags = flags;
        s_lastDrawFlags = flags;
        s_lastDrawWriteMask = key.writeMask[0];
        // Generated indices are swapped on the CPU.
        sys->vertexIndexEndian = indexed && !generateIndices ? (packet.indexSize >> 30) & 3 : 0;
        sys->vertexIndexOffset = Reg(VGT_INDX_OFFSET);
        sys->vertexIndexMin = Reg(VGT_MIN_VTX_INDX);
        sys->vertexIndexMax = Reg(VGT_MAX_VTX_INDX);
        uint32_t colorControl = Reg(RB_COLORCONTROL);
        sys->alphaTestFunc = (colorControl & 8) ? (colorControl & 7) : 7;
        sys->alphaRef = RegF(RB_ALPHA_REF);

        if (LogDraws())
        {
            Scissor s = GetScissor();
            LOGI("draw:   pitch %u msaa %u, rt0 %08X (%ux%u fmt %d) of %u, depth %s ctl %08X, mask %04X blend %08X, cull %u ccw %u, "
                 "vte %03X scale %.1f %.1f %.3f off %.1f %.1f %.3f, scissor %d,%d-%d,%d, alpha %u/%.2f",
                 pitch, msaa, Reg(kColorInfo[0]), colorTargets[0] ? colorTargets[0]->width : 0,
                 colorTargets[0] ? colorTargets[0]->height : 0,
                 colorTargets[0] ? int(colorTargets[0]->viewFormat) : 0, numRT, depthTarget ? "yes" : "no", depthControl,
                 colorMask, key.blend[0], key.cullMode, key.frontCCW, vte, sx, sy, sz, ox, oy, oz, s.left, s.top, s.right,
                 s.bottom, sys->alphaTestFunc, sys->alphaRef);
            LOGI("draw:   constants vs %016llX ps %016llX", (unsigned long long)XXH3_64bits(s_vsBank.raw, sizeof(s_vsBank.raw)),
                 (unsigned long long)XXH3_64bits(s_psBank.raw, sizeof(s_psBank.raw)));
            LOGI("draw:   stencil refmask %08X bf %08X, depth info %08X, mode %08X", Reg(RB_STENCILREFMASK),
                 Reg(RB_STENCILREFMASK_BF), Reg(RB_DEPTH_INFO), mode);
            static uint64_t dumpedAtResolve = ~0ull;
            // MCLA_DUMP_VS=<key>: also dump the constants of every draw with that vertex shader.
            static const uint64_t dumpVs = [] {
                char v[24];
                return GetEnvironmentVariableA("MCLA_DUMP_VS", v, sizeof(v)) > 0 ? strtoull(v, nullptr, 16) : 0ull;
            }();
            if (dumpedAtResolve != s_stats.resolves || vs->key == dumpVs)
            {
                // The first draw after each resolve: its vertex constants in full.
                dumpedAtResolve = s_stats.resolves;
                for (uint32_t c = 0; c < 256; c++)
                {
                    const uint32_t* v = s_vsBank.raw + c * 4;
                    if (v[0] | v[1] | v[2] | v[3])
                    {
                        float f[4];
                        for (int i = 0; i < 4; i++)
                        {
                            uint32_t u = __builtin_bswap32(v[i]);
                            memcpy(&f[i], &u, 4);
                        }
                        LOGI("draw:   c%u = %g %g %g %g", c, f[0], f[1], f[2], f[3]);
                    }
                }
                if (vs->key == dumpVs) // and the pixel constants (logged as 256-511)
                    for (uint32_t c = 0; c < 256; c++)
                    {
                        const uint32_t* v = s_psBank.raw + c * 4;
                        if (v[0] | v[1] | v[2] | v[3])
                        {
                            float f[4];
                            for (int i = 0; i < 4; i++)
                            {
                                uint32_t u = __builtin_bswap32(v[i]);
                                memcpy(&f[i], &u, 4);
                            }
                            LOGI("draw:   c%u = %g %g %g %g", 256 + c, f[0], f[1], f[2], f[3]);
                        }
                    }
            }
            for (uint32_t wd = 0; wd < 3; wd++)
                for (uint32_t bits = vs->vertexFetchSlots[wd]; bits; bits &= bits - 1)
                {
                    uint32_t vfc = wd * 32 + uint32_t(__builtin_ctz(bits));
                    uint32_t d0 = Reg(SHADER_CONSTANT_FETCH_00_0 + vfc * 2);
                    LOGI("draw:   vfetch %u: %08X %08X", vfc, d0, Reg(SHADER_CONSTANT_FETCH_00_0 + vfc * 2 + 1));
                    if (vs->key == dumpVs)
                    {
                        // Raw vertex data (guest order) and the first indices.
                        const uint8_t* data = g_base + 0xA0000000 + (d0 & 0x1FFFFFFC);
                        for (uint32_t row = 0; row < 6; row++)
                        {
                            uint32_t v[9];
                            for (int i = 0; i < 9; i++)
                                v[i] = __builtin_bswap32(reinterpret_cast<const uint32_t*>(data)[row * 9 + i]);
                            LOGI("draw:   vdata %u: %08X %08X %08X %08X %08X %08X %08X %08X %08X", row, v[0], v[1], v[2], v[3],
                                 v[4], v[5], v[6], v[7], v[8]);
                        }
                        if (indexed)
                        {
                            const uint8_t* idx = g_base + 0xA0000000 + (packet.indexBase & 0x1FFFFFFF);
                            LOGI("draw:   indices %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X (endian %u)", idx[0],
                                 idx[1], idx[2], idx[3], idx[4], idx[5], idx[6], idx[7], idx[8], idx[9], idx[10], idx[11],
                                 (packet.indexSize >> 30) & 3);
                        }
                    }
                }
            for (uint32_t slot = 0; slot < 32; slot++)
                if ((vs->textureSlots | ps->textureSlots) & (1u << slot))
                    LOGI("draw:   texture %u -> descriptor %u (fetch %08X %08X %08X %08X)", slot, sys->textureIndex[slot],
                         Reg(SHADER_CONSTANT_FETCH_00_0 + slot * 6), Reg(SHADER_CONSTANT_FETCH_00_0 + slot * 6 + 1),
                         Reg(SHADER_CONSTANT_FETCH_00_0 + slot * 6 + 2), Reg(SHADER_CONSTANT_FETCH_00_0 + slot * 6 + 3));
            if (indexed)
                LOGI("draw:   indices at %08X, endian %u, offset %u min %u max %u", packet.indexBase,
                     sys->vertexIndexEndian, sys->vertexIndexOffset, sys->vertexIndexMin, sys->vertexIndexMax);
        }

        // Record.
        list->SetGraphicsRootSignature(s_rootSignature.Get());
        ID3D12DescriptorHeap* heaps[] = { ViewHeap(), SamplerHeap() };
        list->SetDescriptorHeaps(2, heaps);
        list->SetGraphicsRootConstantBufferView(kRootVSConstants, vsConstants);
        list->SetGraphicsRootConstantBufferView(kRootPSConstants, psConstants);
        list->SetGraphicsRootConstantBufferView(kRootSystemConstants, systemSpace.gpu);
        list->SetGraphicsRootShaderResourceView(kRootMemory, SharedMemoryBuffer()->GetGPUVirtualAddress());
        list->SetGraphicsRootDescriptorTable(kRootTextures, GpuHandle(HeapKind::kView, 0));
        list->SetGraphicsRootDescriptorTable(kRootSamplers, GpuHandle(HeapKind::kSampler, 0));
        list->SetPipelineState(pipeline);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = depthTarget ? CpuHandle(HeapKind::kDsv, depthTarget->view) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        list->OMSetRenderTargets(numRT, numRT ? rtvs : nullptr, FALSE, depthTarget ? &dsv : nullptr);
        float blendFactor[4] = { RegF(RB_BLEND_RED), RegF(RB_BLEND_RED + 1), RegF(RB_BLEND_RED + 2), RegF(RB_BLEND_RED + 3) };
        list->OMSetBlendFactor(blendFactor);
        list->OMSetStencilRef(Reg(RB_STENCILREFMASK) & 0xFF);
        D3D12_VIEWPORT viewport{ 0, 0, w, h, 0.0f, 1.0f };
        list->RSSetViewports(1, &viewport);
        Scissor sc = GetScissor();
        LONG scaleX = LONG(MsaaScaleX(msaa)), scaleY = LONG(MsaaScaleY(msaa));
        D3D12_RECT rect{ std::min<LONG>(sc.left * scaleX, LONG(w)), std::min<LONG>(sc.top * scaleY, LONG(h)),
                         std::min<LONG>(sc.right * scaleX, LONG(w)), std::min<LONG>(sc.bottom * scaleY, LONG(h)) };
        list->RSSetScissorRects(1, &rect);

        D3D12_PRIMITIVE_TOPOLOGY topology;
        switch (hostPrim)
        {
        case 1: topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; break;
        case 2: topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST; break;
        case 3: topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
        case 6: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
        case 13: topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ; break;
        default: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break; // lists, rectangle lists
        }
        list->IASetPrimitiveTopology(topology);
        if (LogDraws())
            RecordPick(list, colorTargets[0], vs->key, ps->key, true);
        if (generateIndices)
        {
            uint32_t outCount = prim == 12 ? count + 1 : (count - 2) * 3;
            UploadSpace space = AllocateUpload(outCount * 4, 4);
            uint32_t* out = reinterpret_cast<uint32_t*>(space.cpu);
            const uint8_t* guestIndices = g_base + 0xA0000000 + (packet.indexBase & 0x1FFFFFFF);
            uint32_t endian = (packet.indexSize >> 30) & 3;
            auto index = [&](uint32_t i) -> uint32_t {
                if (!indexed)
                    return i;
                if (!index32)
                {
                    uint16_t v;
                    memcpy(&v, guestIndices + i * 2, 2);
                    return endian ? __builtin_bswap16(v) : v;
                }
                uint32_t v;
                memcpy(&v, guestIndices + i * 4, 4);
                switch (endian)
                {
                case 1: return ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);
                case 2: return __builtin_bswap32(v);
                case 3: return (v >> 16) | (v << 16);
                default: return v;
                }
            };
            if (prim == 12)
            {
                for (uint32_t i = 0; i < count; i++)
                    out[i] = index(i);
                out[count] = index(0); // (out is write-combined upload memory: don't read it)
            }
            else
            {
                uint32_t first = index(0);
                for (uint32_t i = 0; i + 2 < count; i++)
                {
                    out[i * 3] = first;
                    out[i * 3 + 1] = index(i + 1);
                    out[i * 3 + 2] = index(i + 2);
                }
            }
            D3D12_INDEX_BUFFER_VIEW ibv{ space.gpu, outCount * 4, DXGI_FORMAT_R32_UINT };
            list->IASetIndexBuffer(&ibv);
            list->DrawIndexedInstanced(outCount, 1, 0, 0, 0);
        }
        else if (indexed)
        {
            D3D12_INDEX_BUFFER_VIEW ibv{ SharedMemoryBuffer()->GetGPUVirtualAddress() + (packet.indexBase & 0x1FFFFFFF),
                                         indexBytes, index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT };
            list->IASetIndexBuffer(&ibv);
            if (streamOut)
            {
                s_streamOut.viewportWidth = w;
                s_streamOut.viewportHeight = h;
                if (depthTarget && soKey != ~0ull)
                {
                    LOGI("draw:   depth info %08X (%s)", Reg(RB_DEPTH_INFO), depthTarget->resourceFormat == DXGI_FORMAT_R24G8_TYPELESS ? "D24S8" : "D32S8");
                    CaptureDepthBeforeStreamOut(list, depthTarget->resource.Get(), depthTarget->state);
                    s_streamOut.viewportWidth = w;
                    s_streamOut.viewportHeight = h;
                }
                BeginStreamOut(list);
            }
            list->DrawIndexedInstanced(count, 1, 0, 0, 0);
            if (streamOut)
                EndStreamOut(list);
        }
        else
        {
            if (streamOut)
            {
                s_streamOut.viewportWidth = w;
                s_streamOut.viewportHeight = h;
                BeginStreamOut(list);
            }
            list->DrawInstanced(count, 1, 0, 0);
            if (streamOut)
                EndStreamOut(list);
        }
        s_stats.drawsIssued++;
        if (LogDraws())
        {
            LOGI("draw:   issued as draw %u", s_frameDrawIndex);
            RecordPick(list, colorTargets[0], vs->key, ps->key, false);
            s_frameDrawIndex++;
        }
    }

    namespace
    {
        void DoResolve()
        {
            s_stats.resolves++;
            ID3D12GraphicsCommandList* list = CommandList();
            uint32_t copyControl = Reg(RB_COPY_CONTROL);
            uint32_t srcSelect = copyControl & 7;
            {
                // MCLA_GPU_PASSES: the draws since the last marker rendered into this surface.
                char label[64];
                uint32_t info = Reg(RB_SURFACE_INFO);
                snprintf(label, sizeof(label), "draws into pitch %u msaa %u", info & 0x3FFF, (info >> 16) & 3);
                GpuMarker(label);
            }
            struct ResolveMarker
            {
                ~ResolveMarker() { GpuMarker("resolve copies"); }
            } resolveMarker;
            bool colorClear = (copyControl >> 8) & 1, depthClear = (copyControl >> 9) & 1;
            uint32_t command = (copyControl >> 20) & 3;

            // Source rectangle: the three vertices D3D writes to vertex fetch 0.
            uint32_t fd0 = Reg(SHADER_CONSTANT_FETCH_00_0), fd1 = Reg(SHADER_CONSTANT_FETCH_00_0 + 1);
            uint32_t vaddr = 0xA0000000 | (fd0 & 0x1FFFFFFC), endian = fd1 & 3;
            float half = (Reg(PA_SU_VTX_CNTL) & 1) == 0 ? 0.5f : 0.0f;
            int32_t fx[3], fy[3];
            for (int i = 0; i < 3; i++)
            {
                uint32_t raw[2] = { __builtin_bswap32(Load32(vaddr + i * 8)), __builtin_bswap32(Load32(vaddr + i * 8 + 4)) };
                for (int c = 0; c < 2; c++)
                {
                    uint32_t v = raw[c];
                    if (endian == 1)
                        v = ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);
                    else if (endian == 2)
                        v = __builtin_bswap32(v);
                    else if (endian == 3)
                        v = (v >> 16) | (v << 16);
                    float f;
                    memcpy(&f, &v, 4);
                    int32_t fixed = int32_t(std::lround((f + half) * 256.0f));
                    (c ? fy : fx)[i] = fixed;
                }
            }
            int32_t x0 = (std::min({ fx[0], fx[1], fx[2] }) + 127) >> 8, y0 = (std::min({ fy[0], fy[1], fy[2] }) + 127) >> 8;
            int32_t x1 = (std::max({ fx[0], fx[1], fx[2] }) + 127) >> 8, y1 = (std::max({ fy[0], fy[1], fy[2] }) + 127) >> 8;
            uint32_t mode = Reg(PA_SU_SC_MODE_CNTL);
            if ((mode >> 16) & 1)
            {
                uint32_t wo = Reg(PA_SC_WINDOW_OFFSET);
                x0 += SignExtend(wo, 15);
                x1 += SignExtend(wo, 15);
                y0 += SignExtend(wo >> 16, 15);
                y1 += SignExtend(wo >> 16, 15);
            }
            Scissor sc = GetScissor();
            x0 = std::clamp(x0, sc.left, sc.right);
            x1 = std::clamp(x1, sc.left, sc.right);
            y0 = std::clamp(y0, sc.top, sc.bottom);
            y1 = std::clamp(y1, sc.top, sc.bottom);
            x0 &= ~7;
            y0 &= ~7;
            x1 = (x1 + 7) & ~7;
            y1 = (y1 + 7) & ~7;

            uint32_t surfaceInfo = Reg(RB_SURFACE_INFO);
            uint32_t pitch = surfaceInfo & 0x3FFF, msaa = (surfaceInfo >> 16) & 3;
            bool isDepth = srcSelect >= 4;
            RenderTarget* rt;
            if (isDepth)
            {
                uint32_t info = Reg(RB_DEPTH_INFO);
                rt = GetRenderTarget(info & 0xFFF, pitch, msaa, (info >> 16) & 1, true);
            }
            else
            {
                uint32_t info = Reg(kColorInfo[srcSelect & 3]);
                rt = GetRenderTarget(info & 0xFFF, pitch, msaa, (info >> 16) & 0xF, false);
            }
            if (!rt)
                return;
            // The rectangle is in pixels; the surface stores scaleX x scaleY samples per pixel.
            uint32_t scaleX = MsaaScaleX(msaa), scaleY = MsaaScaleY(msaa);
            x1 = std::min<int32_t>(x1, rt->width / scaleX);
            y1 = std::min<int32_t>(y1, rt->height / scaleY);
            bool empty = x0 >= x1 || y0 >= y1;
            if (!isDepth && !empty)
                ClaimTiles(*rt, uint32_t(y1) * scaleY);
            if (LogDraws())
                LOGI("resolve: surface pitch %u msaa %u, src %u (%08X) rect %d,%d-%d,%d -> %08X pitch %08X info %08X, command %u, "
                     "clear %u%u (%08X %08X, lo %08X)",
                     pitch, msaa, srcSelect, isDepth ? Reg(RB_DEPTH_INFO) : Reg(kColorInfo[srcSelect & 3]), x0, y0, x1, y1,
                     Reg(RB_COPY_DEST_BASE), Reg(RB_COPY_DEST_PITCH), Reg(RB_COPY_DEST_INFO), command, colorClear, depthClear,
                     Reg(RB_COLOR_CLEAR), Reg(RB_DEPTH_CLEAR), Reg(RB_COLOR_CLEAR_LO));

            // Copy to the destination.
            uint32_t destBase = Reg(RB_COPY_DEST_BASE) & 0x1FFFFFFF;
            uint32_t destPitch = Reg(RB_COPY_DEST_PITCH);
            uint32_t destW = destPitch & 0x3FFF, destH = (destPitch >> 16) & 0x3FFF;
            if (!empty && command != 3 && destBase && destW && destH)
            {
                uint32_t offsetX = 0, offsetY = 0;
                uint32_t destFormat = (Reg(RB_COPY_DEST_INFO) >> 7) & 0x3F;
                DXGI_FORMAT destResourceFormat, destViewFormat;
                ResolvedFormats(*rt, destResourceFormat, destViewFormat);
                ResolvedTexture* dest = FindResolveParent(destBase, destW, destH, destResourceFormat,
                                                          ResolveBytesPerPixel(destFormat), offsetX, offsetY);
                if (!dest)
                    dest = GetResolvedTexture(destBase, destW, destH, *rt);
                if (LogDraws())
                    LOGI("resolve:   into texture %08X %ux%u (format %d) at %u,%u", dest->phys, dest->width, dest->height,
                         int(dest->viewFormat), offsetX, offsetY);
                uint32_t dx = std::min<uint32_t>(x0 + offsetX, destW), dy = std::min<uint32_t>(y0 + offsetY, destH);
                uint32_t cw = std::min<uint32_t>(x1 - x0, destW - dx), ch = std::min<uint32_t>(y1 - y0, destH - dy);
                if (cw && ch)
                {
                    // RB_COPY_DEST_INFO bits 16-21: the destination's exponent bias (MCLA
                    // resolves its scene with -4, a 1/16 scale its post-processing expects).
                    // Fixed-point destinations also clamp: the surfaces are kept in the
                    // render target's wider host format.
                    uint32_t destInfo = Reg(RB_COPY_DEST_INFO);
                    int32_t expBias = int32_t((destInfo >> 16) & 0x3F);
                    if (expBias & 0x20)
                        expBias -= 64;
                    bool clampUnorm = (destFormat >= 2 && destFormat <= 10) || destFormat == 15;
                    uint32_t convert = isDepth ? 0u : (uint32_t(expBias) & 0xFF) | (clampUnorm ? 0x100u : 0u);
                    if (isDepth || scaleX != 1 || scaleY != 1 || convert)
                    {
                        ShaderResolve(*rt, x0 * scaleX, y0 * scaleY, scaleX, scaleY, *dest, dx, dy, cw, ch, convert);
                    }
                    else
                    {
                        Transition(list, rt->resource.Get(), rt->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                        Transition(list, dest->resource.Get(), dest->state, D3D12_RESOURCE_STATE_COPY_DEST);
                        D3D12_TEXTURE_COPY_LOCATION dst{ dest->resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                        D3D12_TEXTURE_COPY_LOCATION src{ rt->resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                        D3D12_BOX box{ uint32_t(x0), uint32_t(y0), 0, uint32_t(x0) + cw, uint32_t(y0) + ch, 1 };
                        list->CopyTextureRegion(&dst, dx, dy, 0, &src, &box);
                    }

                    // MCLA_DUMP_RESOLVES=1 with MCLA_DRAW_LOG=<frame>: save every resolve of that frame.
                    static const bool dumpResolves = [] {
                        char v[4];
                        return GetEnvironmentVariableA("MCLA_DUMP_RESOLVES", v, sizeof(v)) > 0;
                    }();
                    if (dumpResolves && LogDraws())
                    {
                        static uint32_t index = 0;
                        char name[64];
                        snprintf(name, sizeof(name), "resolve_%03u_%08X_%ux%u", index++, destBase, destW, destH);
                        SaveTexturePng(dest->resource.Get(), dest->state, name);
                    }
                    // MCLA_FLUSH_RESOLVES=1: submit after every resolve, 2: also wait for the GPU (debugging).
                    static const int flushResolves = [] {
                        char v[4];
                        return GetEnvironmentVariableA("MCLA_FLUSH_RESOLVES", v, sizeof(v)) > 0 ? atoi(v) : 0;
                    }();
                    if (flushResolves)
                    {
                        Flush();
                        if (flushResolves > 1)
                            WaitIdle();
                    }
                }
            }

            // Clears that follow the copy.
            if (!empty && (colorClear || depthClear))
            {
                D3D12_RECT rect{ LONG(x0 * scaleX), LONG(y0 * scaleY), LONG(x1 * scaleX), LONG(y1 * scaleY) };
                if (colorClear && !isDepth)
                {
                    Transition(list, rt->resource.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    float color[4];
                    DecodeColorClear((Reg(kColorInfo[srcSelect & 3]) >> 16) & 0xF, Reg(RB_COLOR_CLEAR), Reg(RB_COLOR_CLEAR_LO),
                                     color);
                    list->ClearRenderTargetView(CpuHandle(HeapKind::kRtv, rt->view), color, 1, &rect);
                }
                if (depthClear)
                {
                    uint32_t info = Reg(RB_DEPTH_INFO);
                    RenderTarget* ds = GetRenderTarget(info & 0xFFF, pitch, msaa, (info >> 16) & 1, true);
                    if (ds)
                    {
                        Transition(list, ds->resource.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                        uint32_t d = Reg(RB_DEPTH_CLEAR);
                        D3D12_RECT drect{ rect.left, rect.top, std::min<LONG>(rect.right, ds->width),
                                          std::min<LONG>(rect.bottom, ds->height) };
                        float depthValue = ((info >> 16) & 1) ? Float20e4To32(d >> 8) : float(d >> 8) / 16777215.0f;
                        list->ClearDepthStencilView(CpuHandle(HeapKind::kDsv, ds->view),
                                                    D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depthValue,
                                                    uint8_t(d & 0xFF), 1, &drect);
                    }
                }
            }
        }
    }

    void Swap(uint32_t frontBufferPhys, uint32_t width, uint32_t height)
    {
        ID3D12GraphicsCommandList* list = CommandList();
        ResolvedTexture* front = FindResolved(frontBufferPhys & 0x1FFFFFFF, width, height);
        ID3D12Resource* back = BackBuffer();
        if (front)
        {
            ResolvedTexture& t = *front;
            t.lastUsedFrame = FrameNumber();
            Transition(list, t.resource.Get(), t.state,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = BackBufferRtv();
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            D3D12_RESOURCE_DESC bd = back->GetDesc();
            // Keep the guest aspect ratio inside the window.
            float scale = std::min(float(bd.Width) / float(width ? width : t.width), float(bd.Height) / float(height ? height : t.height));
            float vw = float(width ? width : t.width) * scale, vh = float(height ? height : t.height) * scale;
            D3D12_VIEWPORT vp{ (float(bd.Width) - vw) * 0.5f, (float(bd.Height) - vh) * 0.5f, vw, vh, 0.0f, 1.0f };
            D3D12_RECT sc{ 0, 0, LONG(bd.Width), LONG(bd.Height) };
            const float black[4] = { 0, 0, 0, 1 };
            list->ClearRenderTargetView(rtv, black, 0, nullptr);
            list->SetGraphicsRootSignature(s_blitRootSignature.Get());
            ID3D12DescriptorHeap* heaps[] = { ViewHeap(), SamplerHeap() };
            list->SetDescriptorHeaps(2, heaps);
            list->SetGraphicsRootDescriptorTable(0, GpuHandle(HeapKind::kView, t.srv));
            list->SetPipelineState(s_blitPipeline.Get());
            list->RSSetViewports(1, &vp);
            list->RSSetScissorRects(1, &sc);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);

            // MCLA_SCREENSHOTS_RANGE=<from>-<to>: only frames in that range (bursts for flicker).
            static const std::pair<uint64_t, uint64_t> range = [] {
                char v[32];
                unsigned long long from = 0, to = ~0ull;
                if (GetEnvironmentVariableA("MCLA_SCREENSHOTS_RANGE", v, sizeof(v)) > 0)
                    sscanf(v, "%llu-%llu", &from, &to);
                return std::pair<uint64_t, uint64_t>(from, to);
            }();
            uint64_t frame = s_swapCount + 1;
            if (ScreenshotInterval() && frame % ScreenshotInterval() == 0 && frame >= range.first && frame <= range.second)
                SaveScreenshot(t.resource.Get(), t.state, frame);
        }
        else
        {
            static int logged = 0;
            if (logged++ < 5)
                LOGI("render: no resolved front buffer at %08X yet", frontBufferPhys);
        }
        Present();
        if (LogDraws())
            ReportPicks();
        s_frameDrawIndex = 0;
        SavePipelineLibrary(false);
        TextureCacheEndFrame();
        EvictResolvedTextures();
        if (LogDraws())
            LOGI("swap: front buffer %08X %ux%u", frontBufferPhys, width, height);
        s_swapCount++;
        {
            LARGE_INTEGER now, frequency;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            if (s_lastSwapTime.QuadPart)
            {
                double ms = double(now.QuadPart - s_lastSwapTime.QuadPart) * 1000.0 / double(frequency.QuadPart);
                s_slowestFrameMs = std::max(s_slowestFrameMs, ms);
                if (ms > 50.0)
                {
                    s_slowFrames++;
                    static const bool logHitches = GetEnvironmentVariableA("MCLA_LOG_HITCHES", nullptr, 0) > 0;
                    if (logHitches)
                    {
                        auto toMs = [&](int64_t t) { return double(t) * 1000.0 / double(frequency.QuadPart); };
                        double protectMs, copyMs;
                        uint32_t runs;
                        uint64_t bytes;
                        TakeSharedMemoryFrameStats(protectMs, copyMs, runs, bytes);
                        LOGI("hitch: %.0f ms frame: draws %.1f ms (%u pipelines created in %.1f ms), textures %.1f ms (%llu KB), "
                             "mirror %u runs %llu KB protect %.1f ms copy %.1f ms, %llu write faults",
                             ms, std::max(0.0, toMs(s_stats.drawTicks - s_frameDrawTicks)), s_framePsos, toMs(s_framePsoTicks),
                             TextureLoadMsThisFrame(), (unsigned long long)(TextureUploadBytesThisFrame() >> 10), runs,
                             (unsigned long long)(bytes >> 10), protectMs, copyMs, (unsigned long long)mem::TakeWriteFaults());
                        LOGI("hitch:   sections: resolve %.1f shaders %.1f targets %.1f pipeline %.1f mirror %.1f bindings %.1f issue %.1f ms",
                             toMs(s_sectionTicks[kSecResolve]), toMs(s_sectionTicks[kSecShaders]), toMs(s_sectionTicks[kSecTargets]),
                             toMs(s_sectionTicks[kSecPipeline]), toMs(s_sectionTicks[kSecMirror]), toMs(s_sectionTicks[kSecBindings]),
                             toMs(s_sectionTicks[kSecIssue]));
                    }
                }
            }
            s_lastSwapTime = now;
            s_framePsoTicks = 0;
            s_framePsos = 0;
            s_frameDrawTicks = s_stats.drawTicks;
            memset(s_sectionTicks, 0, sizeof(s_sectionTicks));
            ResetTextureFrameStats();
            {
                double protectMs, copyMs;
                uint32_t runs;
                uint64_t bytes;
                TakeSharedMemoryFrameStats(protectMs, copyMs, runs, bytes);
                mem::TakeWriteFaults();
            }
        }

        if (s_swapCount % 300 == 0)
        {
            LOGI("render: %llu draws (%llu issued; skipped: %llu unknown shader, %llu compiling, %llu failed, %llu "
                 "primitive), %llu resolves, %llu pipelines, %u compiles pending, %llu KB uploaded, %llu KB textures",
                 (unsigned long long)s_stats.draws, (unsigned long long)s_stats.drawsIssued,
                 (unsigned long long)s_stats.skippedShaderUnknown, (unsigned long long)s_stats.skippedShaderPending,
                 (unsigned long long)s_stats.skippedShaderFailed, (unsigned long long)s_stats.skippedPrimitive,
                 (unsigned long long)s_stats.resolves, (unsigned long long)s_stats.pipelines, PendingShaderCompiles(),
                 (unsigned long long)(TakeSharedMemoryUploadBytes() >> 10), (unsigned long long)(TakeTextureUploadBytes() >> 10));
            LARGE_INTEGER now, frequency;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            double gpuMs, waitMs;
            TakeFrameTimings(gpuMs, waitMs);
            if (s_lastStatsTime.QuadPart)
            {
                double frameMs = double(now.QuadPart - s_lastStatsTime.QuadPart) * 1000.0 / double(frequency.QuadPart) / 300.0;
                LOGI("render: %.1f fps (%.1f ms/frame, slowest %.0f ms, %u over 50 ms): GPU %.1f ms, recording draws %.1f ms, "
                     "waiting for GPU %.1f ms per frame; per frame %llu draws, %llu barriers, %llu mirror uploads",
                     1000.0 / frameMs, frameMs, s_slowestFrameMs, s_slowFrames, gpuMs,
                     double(s_stats.drawTicks) * 1000.0 / double(frequency.QuadPart) / 300.0, waitMs / 300.0,
                     (unsigned long long)(s_stats.drawsIssued / 300), (unsigned long long)(s_stats.barriers / 300),
                     (unsigned long long)(s_stats.mirrorCommits / 300));
            }
            s_lastStatsTime = now;
            s_slowestFrameMs = 0;
            s_slowFrames = 0;
            s_stats = {};

            // The game's frame timer (MCLA 0x827D7500; offsets from LARecomp's notes):
            // +0x08 frame delta (scaled), +0x24 ceiling, +0x28 floor, +0x58 unscaled delta.
            auto timerFloat = [](uint32_t offset) {
                uint32_t bits = Load32(0x827D7500 + offset);
                float f;
                memcpy(&f, &bits, 4);
                return f;
            };
            LOGI("render: game timer: delta %.4f, unscaled %.4f, +0x20 %.4f, ceiling %.4f, floor %.5f", timerFloat(0x08),
                 timerFloat(0x58), timerFloat(0x20), timerFloat(0x24), timerFloat(0x28));
        }
    }
}
