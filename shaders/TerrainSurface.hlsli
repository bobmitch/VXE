// =============================================================================
// TerrainSurface.hlsli - material evaluation + PBR lighting for terrain pixels.
//
// Material sources per pixel:
//   procedural : base colour, variation and micro-normal from analytic-
//                derivative 3D noise evaluated in world (noise) space - no
//                projection needed, so it is seam-free on any orientation.
//   image      : triplanar albedo / BC5 normal / ORM, faded in with imageBlend
//                once the streamer has made every map resident.
// Two materials are blended with a height-aware weight so transitions follow
// relief (grass pooling in crevices, rock crests poking through) instead of a
// linear crossfade.
// =============================================================================
#ifndef VXE_TERRAIN_SURFACE_HLSLI
#define VXE_TERRAIN_SURFACE_HLSLI

#include "Common.hlsli"
#include "Noise.hlsli"
#include "Triplanar.hlsli"

static const float PI = 3.14159265359;

struct SurfaceSample
{
    float3 albedo;
    float3 normal;
    float  roughness;
    float  metallic;
    float  ao;
    float  height;     // for height blending, [0, 1]
};

SurfaceSample evaluateMaterial(uint id, float3 noisePos, float3 n, float3 dpdx, float3 dpdy)
{
    TerrainMaterial m = g_Materials[id];
    Triplanar tp = makeTriplanarGrad(noisePos, n, m.textureScale, m.triplanarSharpness, dpdx, dpdy);
    SurfaceSample s;

    // ---- Procedural ---------------------------------------------------------
    float4 macro = fbmD(noisePos, 0.02, 3, 5u);                // tint variation over ~50 m
    float4 micro = fbmD(noisePos, m.noiseScale, 4, 9u);        // detail + derivative for normal
    s.albedo = saturate(m.baseColor + m.colorVariation * (macro.x * 2.0 + micro.x));
    if (m.pattern == 2u)        // grass: greener on flats, dryer on slopes
        s.albedo *= lerp(float3(0.85, 0.8, 0.6), float3(1.0, 1.05, 0.9), saturate(n.y));
    else if (m.pattern == 4u)   // snow: sparkle-free, slightly blue in cavities
        s.albedo = lerp(s.albedo, s.albedo * float3(0.85, 0.9, 1.0), saturate(-micro.x));
    float3 grad = micro.yzw - n * dot(micro.yzw, n);            // tangential part of the noise gradient
    s.normal = normalize(n - grad * 0.15);
    s.roughness = m.roughness;
    s.metallic = m.metallic;
    s.ao = 1.0;
    s.height = saturate(micro.x + 0.5);
    [branch] if (m.strataFrequency > 0.0)
        s.albedo *= 1.0 + strata(noisePos, m.strataFrequency) * 0.25 * (1.0 - abs(n.y));

    // ---- Image maps (crossfaded) -------------------------------------------
    [branch] if (m.imageBlend > 0.0 && m.albedoSlice != NO_TEXTURE_SLICE)
    {
        float3 imgAlbedo = triplanarSample(g_AlbedoArray, g_LinearWrap, m.albedoSlice, tp).rgb;
        imgAlbedo *= lerp(1.0, 1.0 + macro.x, m.macroVariation);   // anti-tiling macro tint
        float3 imgNormal = s.normal;
        if (m.normalSlice != NO_TEXTURE_SLICE)
            imgNormal = triplanarNormal(g_NormalArray, g_LinearWrap, m.normalSlice, tp, n, 1.0);
        float3 orm = float3(1.0, m.roughness, m.metallic);
        if (m.ormSlice != NO_TEXTURE_SLICE)
            orm = triplanarSample(g_OrmArray, g_LinearWrap, m.ormSlice, tp).rgb;
        s.albedo = lerp(s.albedo, imgAlbedo, m.imageBlend);
        s.normal = normalize(lerp(s.normal, imgNormal, m.imageBlend));
        s.ao = lerp(s.ao, orm.r, m.imageBlend);
        s.roughness = lerp(s.roughness, orm.g, m.imageBlend);
        s.metallic = lerp(s.metallic, orm.b, m.imageBlend);
    }
    return s;
}

SurfaceSample heightBlend(SurfaceSample a, SurfaceSample b, float w, float sharpness)
{
    // Height-lerp (Mikkelsen style): the higher surface wins near the transition.
    float ha = a.height * (1.0 - w), hb = b.height * w;
    float k = saturate((hb - ha) * sharpness + 0.5);
    k = lerp(w, k, step(0.001, w) * step(w, 0.999));
    SurfaceSample r;
    r.albedo = lerp(a.albedo, b.albedo, k);
    r.normal = normalize(lerp(a.normal, b.normal, k));
    r.roughness = lerp(a.roughness, b.roughness, k);
    r.metallic = lerp(a.metallic, b.metallic, k);
    r.ao = lerp(a.ao, b.ao, k);
    r.height = lerp(a.height, b.height, k);
    return r;
}

// ---- GGX / Smith / Schlick --------------------------------------------------
float D_GGX(float NoH, float a) { float a2 = a * a; float d = NoH * NoH * (a2 - 1.0) + 1.0; return a2 / (PI * d * d); }
float V_SmithGGXCorrelated(float NoV, float NoL, float a)
{
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}
float3 F_Schlick(float VoH, float3 f0) { return f0 + (1.0 - f0) * pow(1.0 - VoH, 5.0); }

float3 shadeSurface(SurfaceSample s, float3 viewDir, float cavity)
{
    float3 L = normalize(-g_Frame.sunDirection);
    float3 H = normalize(viewDir + L);
    float NoL = saturate(dot(s.normal, L));
    float NoV = max(dot(s.normal, viewDir), 1e-4);
    float NoH = saturate(dot(s.normal, H));
    float VoH = saturate(dot(viewDir, H));
    float a = max(s.roughness * s.roughness, 0.002);
    float3 f0 = lerp(0.04, s.albedo, s.metallic);
    float3 F = F_Schlick(VoH, f0);
    float3 spec = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoL, a) * F;
    float3 diff = (1.0 - F) * (1.0 - s.metallic) * s.albedo / PI;
    float occlusion = s.ao * lerp(1.0, 0.55, cavity);        // DC cavity term from solid-corner count
    float3 ambient = g_Frame.ambientColor * s.albedo * occlusion * (0.6 + 0.4 * saturate(s.normal.y));
    return (diff + spec) * g_Frame.sunColor * NoL + ambient;
}

#endif // VXE_TERRAIN_SURFACE_HLSLI
