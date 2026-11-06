// =============================================================================
// TerrainTess.hlsl - hardware tessellation path (VS -> HS -> DS -> PS).
//
//   dxc -T vs_6_1 -E VSMain TerrainTess.hlsl
//   dxc -T hs_6_1 -E HSMain TerrainTess.hlsl
//   dxc -T ds_6_1 -E DSMain TerrainTess.hlsl
//   (add -spirv -fspv-target-env=vulkan1.3 for Vulkan)
//
// Tessellation factors are computed per *edge* from the edge's own midpoint
// and length (screen-space target edge size), so the two triangles sharing an
// edge always agree -> crack-free with fractional_odd partitioning. Patches
// beyond displacementFarDistance + fade get factor 1 (pass-through) and
// patches fully outside the frustum get factor 0 (culled before the DS).
//
// Instance id: indirect DrawIndexed args set firstInstance = instance index;
// an identity per-instance vertex stream turns that into INSTANCE_INDEX (works
// identically on D3D12 and Vulkan, unlike SV_InstanceID).
// =============================================================================
#include "TerrainIO.hlsli"
#include "Displacement.hlsli"

#define MAX_DISPLACEMENT_METRES 0.5

struct VSInput
{
    uint vertexId : SV_VertexID;          // includes BaseVertex on both APIs
    uint instance : INSTANCE_INDEX;       // per-instance identity stream
};

struct ControlPoint
{
    float3 posRel   : POSREL;
    float3 noisePos : NOISEPOS;
    float3 normal   : NORMAL;
    nointerpolation uint material : MATERIAL;
};

ControlPoint VSMain(VSInput input)
{
    ChunkInstance inst = g_Instances[input.instance];
    ControlPoint cp;
    loadTerrainVertex(inst, input.vertexId, cp.posRel, cp.noisePos, cp.normal, cp.material);
    return cp;
}

struct PatchConstants
{
    float edges[3] : SV_TessFactor;
    float inside   : SV_InsideTessFactor;
};

float edgeFactor(float3 a, float3 b)
{
    float3 mid = 0.5 * (a + b);
    float dist = max(length(mid), 0.1);
    if (dist > g_Frame.displacementFarDistance + g_Frame.displacementFadeRange) return 1.0;
    float pixels = length(a - b) * g_Frame.projScale / dist;     // projected edge length
    return clamp(pixels / g_Frame.tessTargetPixels, 1.0, g_Frame.tessMaxFactor);
}

bool patchOutsideFrustum(float3 a, float3 b, float3 c)
{
    [unroll] for (int i = 0; i < 6; ++i)
    {
        float4 p = g_Frame.frustumPlanes[i];
        float r = -MAX_DISPLACEMENT_METRES;
        if (dot(p.xyz, a) + p.w < r && dot(p.xyz, b) + p.w < r && dot(p.xyz, c) + p.w < r) return true;
    }
    return false;
}

PatchConstants PatchConstantsMain(InputPatch<ControlPoint, 3> patch)
{
    PatchConstants pc;
    if (patchOutsideFrustum(patch[0].posRel, patch[1].posRel, patch[2].posRel))
    {
        pc.edges[0] = pc.edges[1] = pc.edges[2] = pc.inside = 0.0;
        return pc;
    }
    // D3D convention: edge i is opposite control point i.
    pc.edges[0] = edgeFactor(patch[1].posRel, patch[2].posRel);
    pc.edges[1] = edgeFactor(patch[2].posRel, patch[0].posRel);
    pc.edges[2] = edgeFactor(patch[0].posRel, patch[1].posRel);
    pc.inside = (pc.edges[0] + pc.edges[1] + pc.edges[2]) / 3.0;
    return pc;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_ccw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstantsMain")]
[maxtessfactor(16.0)]
ControlPoint HSMain(InputPatch<ControlPoint, 3> patch, uint i : SV_OutputControlPointID)
{
    return patch[i];
}

[domain("tri")]
TerrainPSInput DSMain(PatchConstants pc, float3 bary : SV_DomainLocation, const OutputPatch<ControlPoint, 3> patch)
{
    TerrainPSInput o;
    float3 posRel   = patch[0].posRel * bary.x + patch[1].posRel * bary.y + patch[2].posRel * bary.z;
    float3 noisePos = patch[0].noisePos * bary.x + patch[1].noisePos * bary.y + patch[2].noisePos * bary.z;
    float3 n = normalize(patch[0].normal * bary.x + patch[1].normal * bary.y + patch[2].normal * bary.z);

    uint3 mats = uint3(patch[0].material, patch[1].material, patch[2].material);
    displacePoint(posRel, n, noisePos, mats, bary);
    // Flat attribute for the PS: dominant corner (the PS re-blends all three
    // corners of the *rasterised* micro-triangle via SV_Barycentrics).
    uint dominant = bary.x >= bary.y && bary.x >= bary.z ? 0u : (bary.y >= bary.z ? 1u : 2u);
    uint mat = mats[dominant];

    o.position = mul(g_Frame.viewProj, float4(posRel, 1.0));
    o.posRel = posRel;
    o.noisePos = noisePos;
    o.normal = n;
    o.material = mat;
    return o;
}
