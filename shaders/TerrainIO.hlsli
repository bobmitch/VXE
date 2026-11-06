// =============================================================================
// TerrainIO.hlsli - stage interfaces shared by every terrain pipeline path.
// =============================================================================
#ifndef VXE_TERRAIN_IO_HLSLI
#define VXE_TERRAIN_IO_HLSLI

#include "Common.hlsli"

// Rasteriser -> pixel shader. `material` is flat and is re-read per corner in
// the PS with GetAttributeAtVertex + SV_Barycentrics, so material blends are
// smooth across triangles without per-vertex weight expansion.
struct TerrainPSInput
{
    float4 position : SV_Position;
    float3 posRel   : POSREL;       // camera-relative world position
    float3 noisePos : NOISEPOS;     // world position mod NOISE_PERIOD
    float3 normal   : NORMAL;
    nointerpolation uint material : MATERIAL;
};

// Fetch + decode one vertex of a chunk instance.
void loadTerrainVertex(ChunkInstance inst, uint vertexIndex, out float3 posRel, out float3 noisePos,
                       out float3 normal, out uint material)
{
    TerrainVertexData v = decodeVertex(g_Vertices[vertexIndex], inst.positionScale);
    posRel = inst.originCameraRelative + v.localPos;
    noisePos = inst.noiseOrigin + v.localPos;
    normal = v.normal;
    material = v.material;
}

#endif // VXE_TERRAIN_IO_HLSLI
