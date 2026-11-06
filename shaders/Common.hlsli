// =============================================================================
// Common.hlsli - shared data layouts and bindings for the terrain pipelines.
// Layouts mirror include/vxe/render/TerrainRenderer.h, MaterialSystem.h and
// meshing/ChunkMesh.h byte for byte (static_asserts on the C++ side).
//
// Compiles with DXC for DXIL (SM 6.5+) and SPIR-V (-spirv, Vulkan 1.3).
// Slang users: these files are valid Slang as-is (HLSL compatibility mode).
// =============================================================================
#ifndef VXE_COMMON_HLSLI
#define VXE_COMMON_HLSLI

// ---- Data layouts ----------------------------------------------------------
struct ChunkInstance
{
    float3 originCameraRelative;   // chunk origin - camera position (computed in double on the CPU)
    float  positionScale;          // metres covered by the unorm16 position range
    uint   vertexBase;
    uint   indexBase;
    uint   meshletBase;
    uint   meshletCount;
    uint   meshletVertexBase;
    uint   meshletTriangleBase;
    uint   lod;
    uint   indexCount;
    float3 noiseOrigin;            // chunk origin mod NOISE_PERIOD (exact)
    uint   flags;
};

struct Meshlet
{
    uint vertexOffset;
    uint triangleOffset;           // in packed triangles (one uint per triangle)
    uint vertexCount;
    uint triangleCount;
};

struct MeshletBounds
{
    float3 center;
    float  radius;
    float3 coneApex;
    float  coneCutoff;
    float3 coneAxis;
    float  pad;
};

struct TerrainMaterial
{
    float3 baseColor;      float roughness;
    float3 colorVariation; float metallic;
    float  noiseScale;     float textureScale;   float triplanarSharpness; float imageBlend;
    uint   albedoSlice;    uint  normalSlice;    uint  ormSlice;           uint  heightSlice;
    float  displacementAmplitude; float displacementFrequency; float strataFrequency; float crackDensity;
    uint   pattern;        float macroVariation; float heightBlendSharpness; float pad;
};

#define NO_TEXTURE_SLICE 0xFFFFFFFFu

struct FrameConstants
{
    float4x4 viewProj;            // camera-relative: no translation, reversed-Z infinite projection
    float4   frustumPlanes[6];    // camera-relative, inward normals (xyz, w = d)
    float3   sunDirection;        float time;
    float3   sunColor;            float projScale;          // viewport height / (2 tan(fov/2))
    float3   ambientColor;        float tessTargetPixels;   // desired screen-space edge length
    float    tessMaxFactor;       float displacementFarDistance; float displacementFadeRange; float nearFieldDistance;
    float3   fogColor;            float fogDensity;
};

struct DrawConstants              // root constants (D3D12 ExecuteIndirect) / push constants (Vulkan)
{
    uint instanceIndex;
    uint nearFieldLevel;          // compute displacement: subdivision level (1..3)
    uint pad0, pad1;
};

// ---- Bindings ----------------------------------------------------------------
ConstantBuffer<FrameConstants>               g_Frame            : register(b0, space0);
#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<DrawConstants> g_Draw     : register(b1, space0);
#else
ConstantBuffer<DrawConstants>                g_Draw             : register(b1, space0);  // root constants
#endif

StructuredBuffer<uint4>          g_Vertices         : register(t0, space0);
StructuredBuffer<uint>           g_Indices          : register(t1, space0);
StructuredBuffer<Meshlet>        g_Meshlets         : register(t2, space0);
StructuredBuffer<uint>           g_MeshletVertices  : register(t3, space0);
StructuredBuffer<uint>           g_MeshletTriangles : register(t4, space0);
StructuredBuffer<MeshletBounds>  g_MeshletBounds    : register(t5, space0);
StructuredBuffer<ChunkInstance>  g_Instances        : register(t6, space0);
StructuredBuffer<TerrainMaterial> g_Materials       : register(t7, space0);
Texture2DArray<float4>           g_AlbedoArray      : register(t8, space0);
Texture2DArray<float4>           g_NormalArray      : register(t9, space0);
Texture2DArray<float4>           g_OrmArray         : register(t10, space0);
Texture2DArray<float>            g_HeightArray      : register(t11, space0);
SamplerState                     g_LinearWrap       : register(s0, space0);

// ---- Vertex decoding -------------------------------------------------------
struct TerrainVertexData
{
    float3 localPos;    // chunk-local metres
    float3 normal;
    uint   material;    // packed: m0 | m1 << 8 | blend << 16 | cavity << 24
};

float3 decodeOctahedral(int2 e)
{
    float2 f = float2(e) / 32767.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

TerrainVertexData decodeVertex(uint4 raw, float positionScale)
{
    TerrainVertexData v;
    float3 q = float3(raw.x & 0xFFFFu, raw.x >> 16, raw.y & 0xFFFFu);
    v.localPos = q * (positionScale / 65535.0);
    int2 oct = int2(int(raw.z << 16) >> 16, int(raw.z) >> 16);   // sign-extend two int16
    v.normal = decodeOctahedral(oct);
    v.material = raw.w;
    return v;
}

uint  materialPrimary(uint m)   { return m & 0xFFu; }
uint  materialSecondary(uint m) { return (m >> 8) & 0xFFu; }
float materialBlend(uint m)     { return float((m >> 16) & 0xFFu) / 255.0; }
float materialCavity(uint m)    { return float(m >> 24) / 255.0; }

#endif // VXE_COMMON_HLSLI
