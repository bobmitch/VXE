// =============================================================================
// Terrain.ps.hlsl - shared pixel shader for all three terrain paths.
//
// Per-pixel material weights: the three corners of the rasterised triangle
// each carry a packed (primary, secondary, blend) material; SV_Barycentrics +
// GetAttributeAtVertex rebuild a smooth weight per material id, and the two
// strongest materials are evaluated and height-blended. Cost is therefore
// bounded at two material evaluations per pixel regardless of local variety.
//
//   dxc -T ps_6_1 -E PSMain Terrain.ps.hlsl
//   dxc -T ps_6_1 -E PSMain -spirv -fspv-target-env=vulkan1.3 Terrain.ps.hlsl
//     (Vulkan requires VK_KHR_fragment_shader_barycentric)
// =============================================================================
#include "TerrainIO.hlsli"
#include "TerrainSurface.hlsli"

#define SLOTS 6

void addWeight(inout uint ids[SLOTS], inout float w[SLOTS], inout uint used, uint id, float weight)
{
    [unroll] for (uint i = 0; i < SLOTS; ++i)
        if (i < used && ids[i] == id) { w[i] += weight; return; }
    if (used < SLOTS) { ids[used] = id; w[used] = weight; ++used; }
}

float4 PSMain(TerrainPSInput input, float3 bary : SV_Barycentrics, bool frontFace : SV_IsFrontFace) : SV_Target0
{
    float3 n = normalize(input.normal);
    if (!frontFace) n = -n;   // cave ceilings seen from inside overhangs with culling disabled

    // Uniform-control-flow derivatives for every texture lookup below.
    float3 dpdx = ddx(input.noisePos);
    float3 dpdy = ddy(input.noisePos);

    uint ids[SLOTS] = {0, 0, 0, 0, 0, 0};
    float w[SLOTS] = {0, 0, 0, 0, 0, 0};
    uint used = 0;
    float cavity = 0.0;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        uint m = GetAttributeAtVertex(input.material, c);
        float b = materialBlend(m);
        addWeight(ids, w, used, materialPrimary(m), bary[c] * (1.0 - b));
        addWeight(ids, w, used, materialSecondary(m), bary[c] * b);
        cavity += bary[c] * materialCavity(m);
    }

    // Two strongest materials.
    uint bestA = 0, bestB = 0;
    float wa = -1.0, wb = -1.0;
    [unroll] for (uint i = 0; i < SLOTS; ++i)
    {
        if (i >= used) break;
        if (w[i] > wa) { wb = wa; bestB = bestA; wa = w[i]; bestA = i; }
        else if (w[i] > wb) { wb = w[i]; bestB = i; }
    }

    SurfaceSample s = evaluateMaterial(ids[bestA], input.noisePos, n, dpdx, dpdy);
    [branch] if (wb > 0.02)
    {
        SurfaceSample s2 = evaluateMaterial(ids[bestB], input.noisePos, n, dpdx, dpdy);
        float t = wb / (wa + wb);
        s = heightBlend(s, s2, t, g_Materials[ids[bestA]].heightBlendSharpness);
    }

    float3 viewDir = normalize(-input.posRel);      // camera at the origin
    float3 color = shadeSurface(s, viewDir, cavity);

    float fog = 1.0 - exp(-g_Frame.fogDensity * length(input.posRel));
    color = lerp(color, g_Frame.fogColor, saturate(fog));
    return float4(color, 1.0);
}
