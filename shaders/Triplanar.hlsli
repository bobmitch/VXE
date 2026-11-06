// =============================================================================
// Triplanar.hlsli - projection mapping for arbitrary volumetric surfaces
// (cliffs, overhangs, cave ceilings) where no UV parameterisation exists.
//
// * Weights: |n|^k, normalised; k (triplanarSharpness) narrows the blend band.
// * UVs are mirrored per axis by the normal sign so textures are never
//   mirrored on back faces (cave ceilings, overhang undersides).
// * Normal maps use the Whiteout blend in world space (Golus 2017): each
//   projection's tangent normal is swizzled into its axis frame and combined
//   with the geometric normal, which is correct for any orientation.
// * Derivatives: UV gradients are derived analytically from ddx/ddy of the
//   noise-space position, taken once in uniform control flow by the caller.
//   Per-pixel material ids are divergent, so calling ddx() inside material
//   branches would be undefined; this keeps anisotropic filtering correct and
//   avoids mip seams at axis switches.
// =============================================================================
#ifndef VXE_TRIPLANAR_HLSLI
#define VXE_TRIPLANAR_HLSLI

struct Triplanar
{
    float3 weights;
    float2 uvX, uvY, uvZ;
    float2 dxX, dyX, dxY, dyY, dxZ, dyZ;   // UV gradients per projection
    float3 axisSign;
};

float2 projX(float3 q, float3 s) { return float2(q.z * s.x, q.y); }
float2 projY(float3 q, float3 s) { return float2(q.x * s.y, q.z); }
float2 projZ(float3 q, float3 s) { return float2(-q.x * s.z, q.y); }

// dpdx/dpdy: screen derivatives of p (zero in DS/CS, where SampleLevel is used).
Triplanar makeTriplanarGrad(float3 p, float3 n, float tileMetres, float sharpness, float3 dpdx, float3 dpdy)
{
    Triplanar t;
    float3 w = pow(abs(n), sharpness);
    t.weights = w / max(w.x + w.y + w.z, 1e-5);
    t.axisSign = float3(n.x < 0.0 ? -1.0 : 1.0, n.y < 0.0 ? -1.0 : 1.0, n.z < 0.0 ? -1.0 : 1.0);
    float inv = 1.0 / tileMetres;
    float3 q = p * inv, qx = dpdx * inv, qy = dpdy * inv;
    t.uvX = projX(q, t.axisSign); t.dxX = projX(qx, t.axisSign); t.dyX = projX(qy, t.axisSign);
    t.uvY = projY(q, t.axisSign); t.dxY = projY(qx, t.axisSign); t.dyY = projY(qy, t.axisSign);
    t.uvZ = projZ(q, t.axisSign); t.dxZ = projZ(qx, t.axisSign); t.dyZ = projZ(qy, t.axisSign);
    return t;
}
Triplanar makeTriplanar(float3 p, float3 n, float tileMetres, float sharpness)
{
    return makeTriplanarGrad(p, n, tileMetres, sharpness, 0.0, 0.0);
}

// Pixel shader path (explicit gradients from makeTriplanarGrad).
float4 triplanarSample(Texture2DArray<float4> tex, SamplerState s, uint slice, Triplanar t)
{
    float4 c = 0.0;
    c += tex.SampleGrad(s, float3(t.uvX, slice), t.dxX, t.dyX) * t.weights.x;
    c += tex.SampleGrad(s, float3(t.uvY, slice), t.dxY, t.dyY) * t.weights.y;
    c += tex.SampleGrad(s, float3(t.uvZ, slice), t.dxZ, t.dyZ) * t.weights.z;
    return c;
}

// Domain / compute shader path (no derivatives available): explicit LOD.
float triplanarSampleLevel(Texture2DArray<float> tex, SamplerState s, uint slice, Triplanar t, float lod)
{
    float h = 0.0;
    h += tex.SampleLevel(s, float3(t.uvX, slice), lod) * t.weights.x;
    h += tex.SampleLevel(s, float3(t.uvY, slice), lod) * t.weights.y;
    h += tex.SampleLevel(s, float3(t.uvZ, slice), lod) * t.weights.z;
    return h;
}

float3 unpackNormalBC5(float4 v)
{
    float2 xy = v.xy * 2.0 - 1.0;
    return float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
}

// World-space normal from three tangent-space normal map projections.
float3 triplanarNormal(Texture2DArray<float4> tex, SamplerState s, uint slice, Triplanar t, float3 n, float strength)
{
    float3 tnX = unpackNormalBC5(tex.SampleGrad(s, float3(t.uvX, slice), t.dxX, t.dyX));
    float3 tnY = unpackNormalBC5(tex.SampleGrad(s, float3(t.uvY, slice), t.dxY, t.dyY));
    float3 tnZ = unpackNormalBC5(tex.SampleGrad(s, float3(t.uvZ, slice), t.dxZ, t.dyZ));
    tnX.xy *= strength; tnY.xy *= strength; tnZ.xy *= strength;
    // Undo the UV mirroring on the tangent X axis.
    tnX.x *= t.axisSign.x;
    tnY.x *= t.axisSign.y;
    tnZ.x *= -t.axisSign.z;
    // Whiteout blend: swizzle into each projection's world frame.
    tnX = float3(tnX.xy + n.zy, abs(tnX.z) * n.x);
    tnY = float3(tnY.xy + n.xz, abs(tnY.z) * n.y);
    tnZ = float3(tnZ.xy + n.xy, abs(tnZ.z) * n.z);
    return normalize(tnX.zyx * t.weights.x + tnY.xzy * t.weights.y + tnZ.xyz * t.weights.z);
}

#endif // VXE_TRIPLANAR_HLSLI
