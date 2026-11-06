// =============================================================================
// TerrainDisplace.cs.hlsl - compute micro-displacement for the mesh-shader
// path (hardware tessellation is not available in the mesh pipeline).
//
// For every near-field LOD0 chunk (TerrainFrame::nearFieldInstances):
//   CSDisplace   : one thread per source triangle. Uniformly subdivides it
//                  into 4^level micro-triangles on a barycentric lattice,
//                  displaces each lattice point once (Displacement.hlsli - the
//                  same function the tessellation DS uses), and appends the
//                  micro-triangles to a non-indexed vertex stream.
//   CSWriteArgs  : turns the atomic vertex counter into DrawInstanced args.
// The stream is drawn with TerrainNearField.hlsl's VS + Terrain.ps.hlsl.
//
// Crack-freedom: every near-field chunk uses the same level in a frame, and
// displacement fades to zero (displacementFarDistance + fadeRange) inside the
// near-field radius, so the subdivided border meets un-subdivided far
// geometry on a straight, undisplaced edge.
//
//   dxc -T cs_6_5 -E CSDisplace  TerrainDisplace.cs.hlsl
//   dxc -T cs_6_5 -E CSWriteArgs TerrainDisplace.cs.hlsl
// =============================================================================
#include "TerrainIO.hlsli"
#include "Displacement.hlsli"

#define MAX_LEVEL 2                     // 4 x 4 subdivision -> 16 micro-triangles
#define MAX_N (1 << MAX_LEVEL)
#define MAX_LATTICE ((MAX_N + 1) * (MAX_N + 2) / 2)

struct DisplacedVertex
{
    float3 posRel;   uint material;
    float3 normal;   float pad0;
    float3 noisePos; float pad1;
};

RWStructuredBuffer<DisplacedVertex> g_OutVertices : register(u0, space0);
RWByteAddressBuffer                 g_Counters    : register(u1, space0);   // [0]: vertices written
RWByteAddressBuffer                 g_DrawArgs    : register(u2, space0);   // DrawInstanced args

uint latticeIndex(uint a, uint b, uint n)    // row a (0..n), column b (0..n-a)
{
    return a * (n + 1) - (a * (a - 1)) / 2 + b;
}

[numthreads(64, 1, 1)]
void CSDisplace(uint tid : SV_DispatchThreadID)
{
    ChunkInstance inst = g_Instances[g_Draw.instanceIndex];
    if (tid * 3 >= inst.indexCount) return;

    float3 P[3], Q[3], N[3];
    uint3 mats;
    [unroll] for (uint k = 0; k < 3; ++k)
    {
        uint vi = inst.vertexBase + g_Indices[inst.indexBase + tid * 3 + k];
        uint m;
        loadTerrainVertex(inst, vi, P[k], Q[k], N[k], m);
        mats[k] = m;
    }

    // Cheap triangle frustum cull (inflated by max displacement).
    [unroll] for (int pl = 0; pl < 6; ++pl)
    {
        float4 p = g_Frame.frustumPlanes[pl];
        if (dot(p.xyz, P[0]) + p.w < -0.5 && dot(p.xyz, P[1]) + p.w < -0.5 && dot(p.xyz, P[2]) + p.w < -0.5) return;
    }

    const uint level = min(g_Draw.nearFieldLevel, (uint)MAX_LEVEL);
    const uint n = 1u << level;
    const uint vertsOut = n * n * 3;
    const uint capacity = g_Draw.pad0;               // vertex capacity of g_OutVertices

    uint base;
    g_Counters.InterlockedAdd(0, vertsOut, base);
    if (base + vertsOut > capacity) return;           // overflow: frame drops micro-detail, never corrupts

    // Displace every lattice point exactly once.
    DisplacedVertex lattice[MAX_LATTICE];
    for (uint a = 0; a <= n; ++a)
        for (uint b = 0; b <= n - a; ++b)
        {
            float3 w = float3(float(n - a - b), float(a), float(b)) / float(n);
            DisplacedVertex v;
            v.posRel   = P[0] * w.x + P[1] * w.y + P[2] * w.z;
            v.noisePos = Q[0] * w.x + Q[1] * w.y + Q[2] * w.z;
            v.normal   = normalize(N[0] * w.x + N[1] * w.y + N[2] * w.z);
            displacePoint(v.posRel, v.normal, v.noisePos, mats, w);
            v.material = mats[w.x >= w.y && w.x >= w.z ? 0 : (w.y >= w.z ? 1 : 2)];
            v.pad0 = 0; v.pad1 = 0;
            lattice[latticeIndex(a, b, n)] = v;
        }

    // Emit micro-triangles with the source triangle's winding.
    uint o = base;
    for (uint r = 0; r < n; ++r)
        for (uint c = 0; c < n - r; ++c)
        {
            uint i0 = latticeIndex(r, c, n), i1 = latticeIndex(r + 1, c, n), i2 = latticeIndex(r, c + 1, n);
            g_OutVertices[o++] = lattice[i0]; g_OutVertices[o++] = lattice[i1]; g_OutVertices[o++] = lattice[i2];
            if (c + 1 < n - r)
            {
                uint j0 = latticeIndex(r + 1, c, n), j1 = latticeIndex(r + 1, c + 1, n), j2 = latticeIndex(r, c + 1, n);
                g_OutVertices[o++] = lattice[j0]; g_OutVertices[o++] = lattice[j1]; g_OutVertices[o++] = lattice[j2];
            }
        }
}

[numthreads(1, 1, 1)]
void CSWriteArgs()
{
    uint count = min(g_Counters.Load(0), g_Draw.pad0);
    g_DrawArgs.Store4(0, uint4(count, 1, 0, 0));      // VertexCountPerInstance, InstanceCount, StartVertex, StartInstance
    g_Counters.Store(0, 0);                            // reset for next frame
}
