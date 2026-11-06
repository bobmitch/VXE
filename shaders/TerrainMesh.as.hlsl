// =============================================================================
// TerrainMesh.as.hlsl - amplification (task) shader, one group per 32 meshlets
// of one chunk instance. Culls meshlets against the frustum (bounding sphere,
// inflated by the max displacement) and by normal cone (backface clusters),
// then launches one mesh-shader group per surviving meshlet.
//
//   dxc -T as_6_5 -E ASMain TerrainMesh.as.hlsl            (D3D12)
//   dxc -T as_6_5 -E ASMain -spirv -fspv-target-env=vulkan1.3 TerrainMesh.as.hlsl
//
// Indirect: one DispatchMesh(ceil(meshletCount / 32), 1, 1) per chunk, with
// g_Draw.instanceIndex written by the command signature (root constant) /
// push constant per indirect draw.
// =============================================================================
#include "Common.hlsli"

#define MESHLETS_PER_GROUP 32
#define MAX_DISPLACEMENT_METRES 0.5

struct TerrainPayload
{
    uint instance;
    uint meshletIndices[MESHLETS_PER_GROUP];
};

groupshared TerrainPayload s_Payload;
groupshared uint s_Count;

bool sphereInFrustum(float3 c, float r)
{
    [unroll] for (int i = 0; i < 6; ++i)
        if (dot(g_Frame.frustumPlanes[i].xyz, c) + g_Frame.frustumPlanes[i].w < -r) return false;
    return true;
}

[numthreads(MESHLETS_PER_GROUP, 1, 1)]
void ASMain(uint gtid : SV_GroupThreadID, uint gid : SV_GroupID)
{
    if (gtid == 0) s_Count = 0;
    GroupMemoryBarrierWithGroupSync();

    const uint instanceIndex = g_Draw.instanceIndex;
    ChunkInstance inst = g_Instances[instanceIndex];
    uint local = gid * MESHLETS_PER_GROUP + gtid;

    bool visible = false;
    if (local < inst.meshletCount)
    {
        MeshletBounds b = g_MeshletBounds[inst.meshletBase + local];
        float3 center = inst.originCameraRelative + b.center;   // camera at origin
        visible = sphereInFrustum(center, b.radius + MAX_DISPLACEMENT_METRES);
        // Normal-cone backface test (meshoptimizer convention): cull when the
        // whole cluster faces away from the camera.
        float3 apex = inst.originCameraRelative + b.coneApex;
        if (visible && b.coneCutoff < 1.0)
            visible = dot(normalize(apex), b.coneAxis) < b.coneCutoff;
    }

    // Wave-size independent compaction (groupshared atomic; portable to
    // 8/16/32/64-wide hardware).
    if (visible)
    {
        uint slot;
        InterlockedAdd(s_Count, 1u, slot);
        s_Payload.meshletIndices[slot] = local;
    }
    if (gtid == 0) s_Payload.instance = instanceIndex;
    GroupMemoryBarrierWithGroupSync();

    DispatchMesh(s_Count, 1, 1, s_Payload);
}
