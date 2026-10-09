// Support code for shaders translated from Xenos microcode (render/xenos/translator.cpp).
// Prepended to every translated shader. Bindings match the renderer's root signature.

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

// Float constants: the guest constant registers, vertex 0-255 and pixel 256-511.
cbuffer VSConstants : register(b0) { float4 g_VS[256]; };
cbuffer PSConstants : register(b1) { float4 g_PS[256]; };

// Must match render/xenos/system_constants.h.
cbuffer SystemConstants : register(b2)
{
    uint4 g_Fetch[48];
    uint4 g_Bools[2];
    uint4 g_Loops[8];
    uint4 g_TextureIndex[8];
    uint4 g_SamplerIndex[8];
    float4 g_NdcScale;
    float4 g_NdcOffset;
    uint4 g_VertexIndex; // endian, offset, min, max
    uint g_Flags;
    uint g_AlphaTestFunc;
    float g_AlphaRef;
    uint g_Pad;
    float4 g_PixelScale; // render target samples per guest pixel (xy)
    float4 g_ColorExpBias; // per render target: 2^RB_COLOR_INFO.color_exp_bias
};

#define FLAG_XY_NOT_DIVIDED 1
#define FLAG_Z_NOT_DIVIDED 2
#define FLAG_W_RECIPROCAL 4

// Guest physical memory (the renderer's mirror).
ByteAddressBuffer g_Memory : register(t0, space0);

Texture2D<float4> g_Textures2D[] : register(t0, space1);
Texture3D<float4> g_Textures3D[] : register(t0, space2);
TextureCube<float4> g_TexturesCube[] : register(t0, space3);
SamplerState g_Samplers[] : register(s0, space0);

uint FetchDword(uint i) { return g_Fetch[i >> 2][i & 3]; }
bool BoolConstant(uint i) { return ((g_Bools[i >> 7][(i >> 5) & 3] >> (i & 31)) & 1) != 0; }
uint LoopConstant(uint i) { return g_Loops[i >> 2][i & 3]; }
// Texture slots: bits 0-15 descriptor index, 16-23 per-component conversion
// (2 bits each, see PostFetch), 24-29 the fetch constant's signed exponent bias.
uint TextureIndex(uint slot) { return g_TextureIndex[slot >> 2][slot & 3] & 0xFFFF; }
uint SamplerIndex(uint slot) { return g_SamplerIndex[slot >> 2][slot & 3]; }

// Xenos endian swap modes.
uint Swap(uint v, uint endian)
{
    if (endian == 1)
        v = ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);
    else if (endian == 2)
        v = (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
    else if (endian == 3)
        v = (v << 16) | (v >> 16);
    return v;
}

// ---- Vertex shader input ----

float VertexIndex(uint rawIndex)
{
    uint index = Swap(rawIndex, g_VertexIndex.x);
    index = (index + g_VertexIndex.y) & 0xFFFFFF;
    return float(clamp(index, g_VertexIndex.z, g_VertexIndex.w));
}

// Vertex fetch constant vfc (0-95): dword 0 = address (bytes, low 2 bits type),
// dword 1 = endian (bits 0-1) and size.
uint VertexFetchAddress(uint vfc) { return FetchDword(vfc * 2) & 0x1FFFFFFC; }
uint VertexFetchEndian(uint vfc) { return FetchDword(vfc * 2 + 1) & 3; }

uint LoadVertexDword(uint address, uint endian)
{
    return Swap(g_Memory.Load(address & 0x1FFFFFFC), endian);
}

float UnpackFraction(uint bits, uint width, bool isSigned, bool noZero)
{
    if (isSigned)
    {
        int v = int(bits << (32 - width)) >> (32 - width);
        if (noZero)
            return (float(v) * 2.0 + 1.0) / float((1u << width) - 1);
        return width > 2 ? max(float(v) / float((1u << (width - 1)) - 1), -1.0) : float(v);
    }
    return width > 1 ? float(bits) / float((1u << width) - 1) : float(bits);
}

float UnpackInteger(uint bits, uint width, bool isSigned)
{
    if (isSigned)
        return float(int(bits << (32 - width)) >> (32 - width));
    return float(bits);
}

float4 UnpackPacked(uint2 d, uint4 widths, uint4 offsets, uint4 dwordOf, bool isSigned, bool isInteger, bool noZero,
                    uint components)
{
    float4 r = float4(0, 0, 0, 1);
    [unroll] for (uint i = 0; i < 4; i++)
    {
        if (i < components)
        {
            uint bits = (d[dwordOf[i]] >> offsets[i]) & ((widths[i] == 32) ? 0xFFFFFFFF : ((1u << widths[i]) - 1));
            r[i] = isInteger ? UnpackInteger(bits, widths[i], isSigned) : UnpackFraction(bits, widths[i], isSigned, noZero);
        }
    }
    return r;
}

// One vertex fetch. All arguments except vfc and index are literals at the call
// sites, so the format switch folds away.
float4 VFetch(uint vfc, uint index, uint stride, int offset, uint format, bool isSigned, bool isInteger, bool noZero,
              int expAdjust)
{
    uint address = VertexFetchAddress(vfc) + (index * stride + offset) * 4;
    uint endian = VertexFetchEndian(vfc);
    uint4 d = 0;
    uint dwords = 1;
    switch (format)
    {
    case 34: case 37: case 26: case 32: dwords = 2; break; // 32_32, 32_32_FLOAT, 16x4, 16x4 FLOAT
    case 57: dwords = 3; break;                             // 32_32_32_FLOAT
    case 35: case 38: dwords = 4; break;                    // 32x4, 32x4 FLOAT
    }
    [unroll] for (uint i = 0; i < 4; i++)
        if (i < dwords)
            d[i] = LoadVertexDword(address + i * 4, endian);

    float4 r = float4(0, 0, 0, 1);
    switch (format)
    {
    case 6: // 8_8_8_8
        r = UnpackPacked(d.xy, uint4(8, 8, 8, 8), uint4(0, 8, 16, 24), uint4(0, 0, 0, 0), isSigned, isInteger, noZero, 4);
        break;
    case 7: // 2_10_10_10
        r = UnpackPacked(d.xy, uint4(10, 10, 10, 2), uint4(0, 10, 20, 30), uint4(0, 0, 0, 0), isSigned, isInteger, noZero, 4);
        break;
    case 16: // 10_11_11
        r = UnpackPacked(d.xy, uint4(11, 11, 10, 0), uint4(0, 11, 22, 0), uint4(0, 0, 0, 0), isSigned, isInteger, noZero, 3);
        break;
    case 17: // 11_11_10
        r = UnpackPacked(d.xy, uint4(10, 11, 11, 0), uint4(0, 10, 21, 0), uint4(0, 0, 0, 0), isSigned, isInteger, noZero, 3);
        break;
    case 25: // 16_16
        r = UnpackPacked(d.xy, uint4(16, 16, 0, 0), uint4(0, 16, 0, 0), uint4(0, 0, 0, 0), isSigned, isInteger, noZero, 2);
        break;
    case 26: // 16_16_16_16
        r = UnpackPacked(d.xy, uint4(16, 16, 16, 16), uint4(0, 16, 0, 16), uint4(0, 0, 1, 1), isSigned, isInteger, noZero, 4);
        break;
    case 31: // 16_16_FLOAT
        r = float4(f16tof32(d.x & 0xFFFF), f16tof32(d.x >> 16), 0, 1);
        break;
    case 32: // 16_16_16_16_FLOAT
        r = float4(f16tof32(d.x & 0xFFFF), f16tof32(d.x >> 16), f16tof32(d.y & 0xFFFF), f16tof32(d.y >> 16));
        break;
    case 33: case 34: case 35: // 32, 32_32, 32_32_32_32 (integer data)
    {
        uint n = format == 33 ? 1 : (format == 34 ? 2 : 4);
        [unroll] for (uint i = 0; i < 4; i++)
        {
            if (i < n)
            {
                float v = isSigned ? float(int(d[i])) : float(d[i]);
                if (!isInteger)
                    v *= isSigned ? (1.0 / 2147483647.0) : (1.0 / 4294967295.0);
                r[i] = v;
            }
        }
        break;
    }
    case 36: r = float4(asfloat(d.x), 0, 0, 1); break;                       // 32_FLOAT
    case 37: r = float4(asfloat(d.xy), 0, 1); break;                          // 32_32_FLOAT
    case 57: r = float4(asfloat(d.xyz), 1); break;                            // 32_32_32_FLOAT
    case 38: r = asfloat(d); break;                                           // 32_32_32_32_FLOAT
    }
    if (expAdjust != 0)
        r *= exp2(float(expAdjust));
    return r;
}

// ---- Vertex shader output ----

float4 GuestToHostPosition(float4 p)
{
    if (g_Flags & FLAG_W_RECIPROCAL)
        p.w = rcp(p.w);
    if (g_Flags & FLAG_XY_NOT_DIVIDED)
        p.xy *= p.w;
    if (g_Flags & FLAG_Z_NOT_DIVIDED)
        p.z *= p.w;
    p.xyz = p.xyz * g_NdcScale.xyz + p.www * g_NdcOffset.xyz;
    return p;
}

// ---- Texture fetches ----

// Conversions the host view format could not do (the view already applies the
// fetch constant's swizzle): per component, 1 = signed data read through an
// unsigned 8-bit view, 2 = unsigned biased (2x-1), 3 = gamma to linear.
float4 PostFetch(uint slot, float4 v)
{
    uint bits = g_TextureIndex[slot >> 2][slot & 3] >> 16;
    if (bits == 0)
        return v;
    [unroll] for (uint i = 0; i < 4; i++)
    {
        uint op = (bits >> (i * 2)) & 3;
        float c = v[i];
        if (op == 1)
        {
            float s = round(c * 255.0);
            c = max((s >= 128.0 ? s - 256.0 : s) / 127.0, -1.0);
        }
        else if (op == 2)
        {
            c = c * 2.0 - 1.0;
        }
        else if (op == 3)
        {
            c = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
        }
        v[i] = c;
    }
    int expAdjust = int(bits << 18) >> 26;
    if (expAdjust != 0)
        v *= exp2(float(expAdjust));
    return v;
}

// mode: 0 implicit LOD (pixel shaders), 1 explicit LOD, 2 explicit gradients.
float4 TFetch2D(uint slot, float2 uv, bool unnormalized, float2 offset, uint mode, float lod, float2 gradH, float2 gradV,
                float bias)
{
    Texture2D<float4> t = g_Textures2D[NonUniformResourceIndex(TextureIndex(slot))];
    SamplerState s = g_Samplers[NonUniformResourceIndex(SamplerIndex(slot))];
    float2 size;
    t.GetDimensions(size.x, size.y);
    if (unnormalized)
        uv /= size;
    uv += offset / size;
    if (mode == 1)
        return PostFetch(slot, t.SampleLevel(s, uv, lod));
    if (mode == 2)
        return PostFetch(slot, t.SampleGrad(s, uv, gradH, gradV));
    return PostFetch(slot, t.SampleBias(s, uv, bias));
}

float4 TFetch3D(uint slot, float3 uvw, bool unnormalized, float3 offset, uint mode, float lod, float3 gradH, float3 gradV,
                float bias)
{
    Texture3D<float4> t = g_Textures3D[NonUniformResourceIndex(TextureIndex(slot))];
    SamplerState s = g_Samplers[NonUniformResourceIndex(SamplerIndex(slot))];
    float3 size;
    t.GetDimensions(size.x, size.y, size.z);
    if (unnormalized)
        uvw /= size;
    uvw += offset / size;
    if (mode == 1)
        return PostFetch(slot, t.SampleLevel(s, uvw, lod));
    if (mode == 2)
        return PostFetch(slot, t.SampleGrad(s, uvw, gradH, gradV));
    return PostFetch(slot, t.SampleBias(s, uvw, bias));
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

// The cube ALU instruction's result feeds tfetchCube; the direction it was given
// is kept here and looked up by the index the translated cube() returns in W.
float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index & 1] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    return float4(0.0, 0.0, 0.0, index);
}

float4 TFetchCube(uint slot, float3 coord, inout CubeMapData cubeMapData, uint mode, float lod, float bias)
{
    TextureCube<float4> t = g_TexturesCube[NonUniformResourceIndex(TextureIndex(slot))];
    SamplerState s = g_Samplers[NonUniformResourceIndex(SamplerIndex(slot))];
    float3 dir = cubeMapData.cubeMapDirections[uint(coord.z) & 1];
    if (mode == 1)
        return PostFetch(slot, t.SampleLevel(s, dir, lod));
    return PostFetch(slot, t.SampleBias(s, dir, bias));
}

float GetLod2D(uint slot, float2 uv, bool unnormalized)
{
    Texture2D<float4> t = g_Textures2D[NonUniformResourceIndex(TextureIndex(slot))];
    SamplerState s = g_Samplers[NonUniformResourceIndex(SamplerIndex(slot))];
    if (unnormalized)
    {
        float2 size;
        t.GetDimensions(size.x, size.y);
        uv /= size;
    }
    return t.CalculateLevelOfDetailUnclamped(s, uv);
}

float4 GetWeights2D(uint slot, float2 uv, bool unnormalized, float2 offset)
{
    Texture2D<float4> t = g_Textures2D[NonUniformResourceIndex(TextureIndex(slot))];
    float2 size;
    t.GetDimensions(size.x, size.y);
    float2 texel = unnormalized ? uv : uv * size;
    return float4(frac(texel + offset - 0.5), 0.0, 0.0);
}

// ---- ALU helpers ----

float4 dst(float4 src0, float4 src1)
{
    return float4(1.0, src0.y * src1.y, src0.z, src1.w);
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

// ---- Pixel shader output ----

// The render target's exponent bias scales all four components of the output
// (after the alpha test, before blending): MCLA draws its scene with +4 and
// resolves it with -4, and gives its mirror glass alpha 1/16 to make it opaque.
// Render targets are also wider than the EDRAM formats they stand for: a
// 2_10_10_10_FLOAT surface (stored as 16-bit float) keeps 7e3 color in
// 0..31.875 and a 2-bit alpha.
#define FLAG_COLOR_7E3 256
#define FLAG_COLOR_7E3_UNBLENDED 4096
float4 ColorOutput(float4 c, uint index)
{
    c *= g_ColorExpBias[index];
    if (g_Flags & (FLAG_COLOR_7E3 << index))
    {
        c.rgb = clamp(c.rgb, 0.0, 31.875);
        c.a = saturate(c.a);
        if (g_Flags & (FLAG_COLOR_7E3_UNBLENDED << index))
            c.a = round(c.a * 3.0) / 3.0;
    }
    return c;
}

void AlphaTest(float alpha)
{
    bool pass = true;
    switch (g_AlphaTestFunc)
    {
    case 0: pass = false; break;
    case 1: pass = alpha < g_AlphaRef; break;
    case 2: pass = alpha == g_AlphaRef; break;
    case 3: pass = alpha <= g_AlphaRef; break;
    case 4: pass = alpha > g_AlphaRef; break;
    case 5: pass = alpha != g_AlphaRef; break;
    case 6: pass = alpha >= g_AlphaRef; break;
    }
    if (!pass)
        discard;
}
