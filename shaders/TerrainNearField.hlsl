// =============================================================================
// TerrainNearField.hlsl - vertex-pulling VS for the compute-displaced
// near-field stream (mesh-shader path) and the plain classic-path VS.
//
//   dxc -T vs_6_1 -E VSNearField TerrainNearField.hlsl
//   dxc -T vs_6_1 -E VSClassic   TerrainNearField.hlsl
// =============================================================================
#include "TerrainIO.hlsli"

struct DisplacedVertex
{
    float3 posRel;   uint material;
    float3 normal;   float pad0;
    float3 noisePos; float pad1;
};
StructuredBuffer<DisplacedVertex> g_DisplacedVertices : register(t12, space0);

TerrainPSInput VSNearField(uint vertexId : SV_VertexID)
{
    DisplacedVertex v = g_DisplacedVertices[vertexId];
    TerrainPSInput o;
    o.position = mul(g_Frame.viewProj, float4(v.posRel, 1.0));
    o.posRel = v.posRel;
    o.noisePos = v.noisePos;
    o.normal = v.normal;
    o.material = v.material;
    return o;
}

// Classic path (no tessellation, no mesh shaders): indexed draw per chunk.
TerrainPSInput VSClassic(uint vertexId : SV_VertexID, uint instance : INSTANCE_INDEX)
{
    ChunkInstance inst = g_Instances[instance];
    TerrainPSInput o;
    loadTerrainVertex(inst, vertexId, o.posRel, o.noisePos, o.normal, o.material);
    o.position = mul(g_Frame.viewProj, float4(o.posRel, 1.0));
    return o;
}
