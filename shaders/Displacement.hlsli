// =============================================================================
// Displacement.hlsli - the single definition of terrain micro-relief.
//
// Used by BOTH the hardware tessellation domain shader and the compute
// near-field displacement pass, so the two pipelines produce identical
// geometry. Height is a material-weighted sum of:
//   * multi-frequency fBm          (gravel, lumps)          all materials
//   * cellular crack grooves        (crackDensity)           rock
//   * strata bands on steep faces   (strataFrequency)        rock / deepstone
//   * image height map (triplanar)  when resident, crossfaded by imageBlend
// and is faded to zero between displacementFarDistance and +fadeRange, which
// also makes the edge against non-displaced geometry crack-free.
// =============================================================================
#ifndef VXE_DISPLACEMENT_HLSLI
#define VXE_DISPLACEMENT_HLSLI

#include "Common.hlsli"
#include "Noise.hlsli"
#include "Triplanar.hlsli"

// Signed height in metres for one material (before distance fade).
float materialHeight(TerrainMaterial m, float3 noisePos, float3 n, float lod)
{
    float h = fbm(noisePos, m.displacementFrequency, 3, 11u);             // [-0.5, 0.5]-ish
    [branch] if (m.crackDensity > 0.0)
    {
        float edge = cellularEdge(noisePos, 0.45, 23u);
        h -= m.crackDensity * (1.0 - smoothstep(0.0, 0.12, edge));         // grooves along cell borders
    }
    [branch] if (m.strataFrequency > 0.0)
        h += strata(noisePos, m.strataFrequency) * (1.0 - abs(n.y)) * 0.8; // only on cliff faces

    [branch] if (m.heightSlice != NO_TEXTURE_SLICE && m.imageBlend > 0.0)
    {
        Triplanar t = makeTriplanar(noisePos, n, m.textureScale, m.triplanarSharpness);
        float img = triplanarSampleLevel(g_HeightArray, g_LinearWrap, m.heightSlice, t, lod) - 0.5;
        h = lerp(h, img, m.imageBlend);
    }
    return h * m.displacementAmplitude;
}

float displacementFade(float distanceToCamera)
{
    return 1.0 - saturate((distanceToCamera - g_Frame.displacementFarDistance) / max(g_Frame.displacementFadeRange, 1e-3));
}

// Blended height for a packed vertex material (primary/secondary + blend).
float packedMaterialHeight(uint packedMat, float3 noisePos, float3 n, float lod)
{
    TerrainMaterial m0 = g_Materials[materialPrimary(packedMat)];
    float h = materialHeight(m0, noisePos, n, lod);
    float b = materialBlend(packedMat);
    [branch] if (b > 0.01)
    {
        TerrainMaterial m1 = g_Materials[materialSecondary(packedMat)];
        h = lerp(h, materialHeight(m1, noisePos, n, lod), b);
    }
    return h;
}

// Height at a point inside a triangle whose three corners carry (possibly
// different) packed materials. Weighting by barycentrics keeps the field
// continuous across the patch and across shared edges (the opposite corner's
// weight is zero on an edge), which is what keeps displaced geometry crack-free.
float cornerBlendedHeight(uint3 mats, float3 w, float3 noisePos, float3 n, float lod)
{
    [branch] if (mats.x == mats.y && mats.y == mats.z)
        return packedMaterialHeight(mats.x, noisePos, n, lod);
    return w.x * packedMaterialHeight(mats.x, noisePos, n, lod)
         + w.y * packedMaterialHeight(mats.y, noisePos, n, lod)
         + w.z * packedMaterialHeight(mats.z, noisePos, n, lod);
}

// Displaces a point along its normal and returns the perturbed normal,
// estimated with two tangent-plane finite differences of the same height field.
void displacePoint(inout float3 posRel, inout float3 n, float3 noisePos, uint3 mats, float3 w)
{
    float dist = length(posRel);
    float fade = displacementFade(dist);
    if (fade <= 0.0) return;
    float lod = log2(max(dist, 1.0)) - 1.0;

    float3 t1 = normalize(abs(n.y) < 0.99 ? cross(n, float3(0, 1, 0)) : cross(n, float3(1, 0, 0)));
    float3 t2 = cross(n, t1);
    const float e = 0.05;   // 5 cm probe
    float h0 = cornerBlendedHeight(mats, w, noisePos, n, lod) * fade;
    float h1 = cornerBlendedHeight(mats, w, noisePos + t1 * e, n, lod) * fade;
    float h2 = cornerBlendedHeight(mats, w, noisePos + t2 * e, n, lod) * fade;

    posRel += n * h0;
    n = normalize(n - t1 * ((h1 - h0) / e) - t2 * ((h2 - h0) / e));
}

#endif // VXE_DISPLACEMENT_HLSLI
