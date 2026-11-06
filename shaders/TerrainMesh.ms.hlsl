// =============================================================================
// TerrainMesh.ms.hlsl - mesh shader: one group per visible meshlet.
// Pulls packed vertices (16 B) and packed triangles (one uint each) straight
// from the mega-buffers; outputs camera-relative positions.
//
//   dxc -T ms_6_5 -E MSMain TerrainMesh.ms.hlsl
//   dxc -T ms_6_5 -E MSMain -spirv -fspv-target-env=vulkan1.3 TerrainMesh.ms.hlsl
//
// Near-field micro displacement for this path is produced by
// TerrainDisplace.cs.hlsl into a separate buffer (see TerrainNearField.hlsl);
// meshlets of near-field chunks are skipped here via ChunkInstance.flags.
// =============================================================================
#include "TerrainIO.hlsli"

#define MESHLETS_PER_GROUP 32
#define MAX_VERTS 64
#define MAX_PRIMS 124
#define CHUNK_FLAG_NEAR_FIELD 1u

struct TerrainPayload
{
    uint instance;
    uint meshletIndices[MESHLETS_PER_GROUP];
};

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void MSMain(uint gtid : SV_GroupThreadID, uint gid : SV_GroupID,
            in payload TerrainPayload payload,
            out vertices TerrainPSInput verts[MAX_VERTS],
            out indices uint3 tris[MAX_PRIMS])
{
    ChunkInstance inst = g_Instances[payload.instance];
    Meshlet m = g_Meshlets[inst.meshletBase + payload.meshletIndices[gid]];
    const bool skip = (inst.flags & CHUNK_FLAG_NEAR_FIELD) != 0;
    const uint vcount = skip ? 0 : m.vertexCount;
    const uint pcount = skip ? 0 : m.triangleCount;
    SetMeshOutputCounts(vcount, pcount);

    if (gtid < vcount)
    {
        uint vi = g_MeshletVertices[inst.meshletVertexBase + m.vertexOffset + gtid];
        TerrainPSInput o;
        loadTerrainVertex(inst, inst.vertexBase + vi, o.posRel, o.noisePos, o.normal, o.material);
        o.position = mul(g_Frame.viewProj, float4(o.posRel, 1.0));
        verts[gtid] = o;
    }
    if (gtid < pcount)
    {
        uint packed = g_MeshletTriangles[inst.meshletTriangleBase + m.triangleOffset + gtid];
        tris[gtid] = uint3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
    }
}
