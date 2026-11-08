// =============================================================================
// vxe_viewer - windowed, interactive demo of the VXE terrain pipeline.
//
// The engine ships a CPU NullRenderDevice only (see README "Backend mapping"),
// so this app carries a small, self-contained Direct3D 11 backend that draws
// the mesher's output directly. It is the TerrainPipelinePath::Classic layout
// (plain VS/PS, no tessellation, no mesh shaders): one vertex/index buffer per
// resident chunk, camera-relative positions, frustum-culled with the engine's
// own vxe::Frustum.
//
// What it exercises:
//   DensityGenerator -> ChunkStreamer (octree LOD selection, async build graph)
//   -> DualContouring -> MeshOptimizerStage -> this backend
//   plus Terraformer for live CSG edits (dig / fill under the crosshair).
//
// Cameras:
//   * Flyby   - a closed Catmull-Rom spline laid over the terrain at startup,
//               arc-length re-parameterised so the speed is steady, with a
//               separate look-at spline so the camera leads into the turns.
//   * Free    - WASD/QE + mouse look.
//
// Controls are printed to the console on startup and summarised in the title
// bar together with live streaming stats.
// =============================================================================
#include "vxe/building/TerrainQuery.h"
#include "vxe/core/TaskSystem.h"
#include "vxe/render/MaterialSystem.h"
#include "vxe/render/TerrainRenderer.h"   // vxe::Frustum / vxe::Plane
#include "vxe/terraform/DeltaStore.h"
#include "vxe/terraform/Terraformer.h"
#include "vxe/world/ChunkStreamer.h"
#include "vxe/world/DensityGenerator.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using namespace vxe;
using Clock = std::chrono::steady_clock;

namespace {

// -----------------------------------------------------------------------------
// Tiny COM helper (avoids pulling in wrl/atl)
// -----------------------------------------------------------------------------
template <typename T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p(o.p) { o.p = nullptr; }
    Com& operator=(Com&& o) noexcept {
        if (this != &o) { reset(); p = o.p; o.p = nullptr; }
        return *this;
    }
    ~Com() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    T* get() const { return p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

bool fail(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return false;
    std::fprintf(stderr, "[vxe_viewer] %s failed (hr=0x%08lX)\n", what, (unsigned long)hr);
    return true;
}

// -----------------------------------------------------------------------------
// Matrices. Row-vector convention throughout: clip = v * (view * proj), and the
// HLSL side declares the matrix `row_major`, so no transposes anywhere.
// -----------------------------------------------------------------------------
struct Mat4 {
    float m[4][4]{};
    static Mat4 identity() {
        Mat4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.f;
        return r;
    }
};

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

// Left-handed, reverse-Z, infinite far plane: depth = zNear / zView, so it is 1
// at the near plane and tends to 0 at infinity. Paired with a D32_FLOAT buffer,
// clear-to-0 and DepthFunc GREATER this gives usable precision out to km range,
// which a 0.1 m near plane over a 2 km view distance otherwise would not.
Mat4 perspectiveReverseZInfinite(float fovYRadians, float aspect, float zNear) {
    const float f = 1.f / std::tan(fovYRadians * 0.5f);
    Mat4 p;
    p.m[0][0] = f / aspect;
    p.m[1][1] = f;
    p.m[2][3] = 1.f;
    p.m[3][2] = zNear;
    return p;
}

// Rotation-only view matrix: the camera always sits at the origin of the space
// we render in (positions are camera-relative, computed in double then narrowed).
Mat4 viewFromBasis(const Vec3f& right, const Vec3f& up, const Vec3f& fwd) {
    Mat4 v = Mat4::identity();
    v.m[0][0] = right.x; v.m[0][1] = up.x; v.m[0][2] = fwd.x;
    v.m[1][0] = right.y; v.m[1][1] = up.y; v.m[1][2] = fwd.y;
    v.m[2][0] = right.z; v.m[2][1] = up.z; v.m[2][2] = fwd.z;
    return v;
}

// Gribb/Hartmann plane extraction for the row-vector convention: the planes are
// combinations of the *columns* of viewProj. Near/far come from the camera basis
// instead, because the infinite reverse-Z matrix degenerates there.
Frustum frustumFrom(const Mat4& viewProj, const Vec3f& fwd, float zNear, float viewDistance) {
    auto col = [&](int j) { return Vec3f{viewProj.m[0][j], viewProj.m[1][j], viewProj.m[2][j]}; };
    auto colW = [&](int j) { return viewProj.m[3][j]; };
    auto plane = [](Vec3f n, float d) {
        const float l = length(n);
        return Plane{l > 1e-9f ? n / l : n, l > 1e-9f ? d / l : d};
    };
    Frustum fr{};
    fr.planes[0] = plane(col(3) + col(0), colW(3) + colW(0));   // left
    fr.planes[1] = plane(col(3) - col(0), colW(3) - colW(0));   // right
    fr.planes[2] = plane(col(3) + col(1), colW(3) + colW(1));   // bottom
    fr.planes[3] = plane(col(3) - col(1), colW(3) - colW(1));   // top
    fr.planes[4] = plane(fwd, -zNear);                          // near
    fr.planes[5] = plane(-fwd, viewDistance);                   // far
    return fr;
}

// -----------------------------------------------------------------------------
// Shaders. Compiled at runtime with D3DCompile so the demo needs no build step
// beyond the C++ one; the cooked .hlsl in shaders/ targets the mesh-shader and
// tessellation paths and is not used here.
// -----------------------------------------------------------------------------
const char kShaderSource[] = R"HLSL(
cbuffer Frame : register(b0)
{
    row_major float4x4 gViewProj;
    float3 gSunDir;     float gFogDensity;
    float3 gSunColor;   float gLodTint;
    float3 gSkyTop;     float gTanHalfFovX;
    float3 gSkyHorizon; float gTanHalfFovY;
    float3 gCamRight;   float gPad0;
    float3 gCamUp;      float gPad1;
    float3 gCamFwd;     float gPad2;
    float4 gMaterial[8];   // rgb = base colour, a = roughness (unused by this path)
    float  gTessTargetPx;  float gPixPerRadian;   // tessellation: target edge size, px per radian
    float  gDispFar;       float gDispFade;       // displacement fades out over this band
    float  gDispAmp;       float gTessMax;
    float  gPad3;          float gPad4;
};

cbuffer Chunk : register(b1)
{
    float3 gChunkOffset;   // chunk origin - camera position, double-differenced on the CPU
    float  gChunkLod;
    float3 gChunkWorld;    // chunk origin in world space: displacement must be a
    float  gChunkPad;      // function of world position, not of the camera
};

struct VSIn {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    uint   mat : MATERIAL;
};
struct VSOut {
    float4 clip   : SV_Position;
    float3 camRel : TEXCOORD0;
    float3 nrm    : TEXCOORD1;
    float3 albedo : TEXCOORD2;
    float  ao     : TEXCOORD3;
};

static const float3 kLodTint[7] = {
    float3(0.40, 0.85, 0.40), float3(0.85, 0.85, 0.35), float3(0.90, 0.60, 0.30),
    float3(0.90, 0.35, 0.35), float3(0.75, 0.40, 0.85), float3(0.40, 0.60, 0.95),
    float3(0.60, 0.60, 0.60)
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    const float3 camRel = i.pos + gChunkOffset;
    o.clip   = mul(float4(camRel, 1.0), gViewProj);
    o.camRel = camRel;
    o.nrm    = i.nrm;

    // packMaterial(): [0..7] primary, [8..15] secondary, [16..23] blend, [24..31] cavity/AO
    const uint  primary   = i.mat & 0xFFu;
    const uint  secondary = (i.mat >> 8) & 0xFFu;
    const float blend     = float((i.mat >> 16) & 0xFFu) * (1.0 / 255.0);
    o.ao                  = float((i.mat >> 24) & 0xFFu) * (1.0 / 255.0);

    float3 albedo = lerp(gMaterial[min(primary, 7u)].rgb, gMaterial[min(secondary, 7u)].rgb, blend);
    o.albedo = lerp(albedo, kLodTint[min(uint(gChunkLod), 6u)], gLodTint);
    return o;
}

// Cheap value noise, used only to break up the flat procedural albedo. The real
// material model (triplanar / strata / cracks / image maps) lives in
// shaders/Triplanar.hlsli and runs on the mesh-shader path.
float hash31(float3 p)
{
    p = frac(p * 0.3183099 + float3(0.1, 0.2, 0.3));
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float valueNoise(float3 p)
{
    const float3 i = floor(p), f = frac(p);
    const float3 u = f * f * (3.0 - 2.0 * f);
    const float n000 = hash31(i + float3(0, 0, 0)), n100 = hash31(i + float3(1, 0, 0));
    const float n010 = hash31(i + float3(0, 1, 0)), n110 = hash31(i + float3(1, 1, 0));
    const float n001 = hash31(i + float3(0, 0, 1)), n101 = hash31(i + float3(1, 0, 1));
    const float n011 = hash31(i + float3(0, 1, 1)), n111 = hash31(i + float3(1, 1, 1));
    return lerp(lerp(lerp(n000, n100, u.x), lerp(n010, n110, u.x), u.y),
                lerp(lerp(n001, n101, u.x), lerp(n011, n111, u.x), u.y), u.z);
}

float3 skyColor(float3 dir)
{
    const float t = saturate(dir.y * 0.5 + 0.5);
    float3 sky = lerp(gSkyHorizon, gSkyTop, pow(t, 0.65));
    const float sun = pow(saturate(dot(dir, gSunDir)), 220.0);
    return sky + gSunColor * sun * 1.4;
}

float4 PSMain(VSOut i) : SV_Target
{
    const float3 n = normalize(i.nrm);
    const float3 v = normalize(-i.camRel);
    const float  dist = length(i.camRel);

    // Detail: two octaves in camera-relative space (small floats, no jitter).
    // It has to fade out with distance or the high frequencies alias into moire
    // bands on far geometry - there is no mip chain to filter it for us here.
    const float detailFade = saturate(1.0 - dist / 260.0);
    const float raw = valueNoise(i.camRel * 0.28) * 0.65 + valueNoise(i.camRel * 1.1) * 0.35;
    const float detail = lerp(0.5, raw, detailFade);
    float3 albedo = i.albedo * (0.84 + 0.32 * detail);
    albedo *= lerp(0.62, 1.0, i.ao);

    const float  ndl  = saturate(dot(n, gSunDir));
    const float3 sun  = gSunColor * ndl;
    // Hemisphere ambient + a dim bounce from below, so cave roofs are not black.
    const float3 amb  = lerp(float3(0.10, 0.10, 0.12), gSkyTop * 0.55, saturate(n.y * 0.5 + 0.5));
    // Wrapped specular sheen, enough to read the silhouette of ridges.
    const float3 h    = normalize(gSunDir + v);
    const float  spec = pow(saturate(dot(n, h)), 24.0) * 0.06 * ndl;

    float3 color = albedo * (sun + amb) + spec;

    // Height fog: denser low down, so distant valleys recede before the peaks do.
    const float heightFalloff = clamp(exp(-i.camRel.y * 0.0012), 0.55, 1.8);
    const float fog = 1.0 - exp(-dist * gFogDensity * heightFalloff);
    color = lerp(color, skyColor(normalize(i.camRel)), saturate(fog));

    // Filmic-ish tonemap + gamma.
    color = (color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14);
    return float4(pow(saturate(color), 1.0 / 2.2), 1.0);
}

// Fullscreen sky pass (no vertex buffer; SV_VertexID triangle).
struct SkyOut { float4 clip : SV_Position; float2 ndc : TEXCOORD0; };
SkyOut VSSky(uint vid : SV_VertexID)
{
    SkyOut o;
    o.ndc  = float2((vid << 1) & 2, vid & 2) * 2.0 - 1.0;
    o.clip = float4(o.ndc, 0.0, 1.0);   // reverse-Z: 0 == infinitely far
    return o;
}
float4 PSSky(SkyOut i) : SV_Target
{
    // Rebuild the camera-relative view ray straight from the camera basis; no
    // matrix inverse needed since the camera sits at the origin of this space.
    const float3 dir = normalize(gCamFwd + gCamRight * (i.ndc.x * gTanHalfFovX) +
                                           gCamUp    * (i.ndc.y * gTanHalfFovY));
    return float4(pow(saturate(skyColor(dir)), 1.0 / 2.2), 1.0);
}
)HLSL"
// MSVC caps one string literal at 16380 bytes, so the source carries on in a
// second adjacent literal. They concatenate into a single array, so
// sizeof(kShaderSource) still spans both.
R"HLSL(
// ---- On-screen overlay ------------------------------------------------------
cbuffer Overlay : register(b3) { float4 gOverlayRect; };   // x0, y0, x1, y1 in NDC
Texture2D    gOverlayTex : register(t2);
SamplerState gOverlaySmp : register(s1);

struct OverlayOut { float4 clip : SV_Position; float2 uv : TEXCOORD0; };

OverlayOut VSOverlay(uint vid : SV_VertexID)
{
    OverlayOut o;
    const float2 c = float2((vid << 1) & 2, vid & 2);          // (0,0) (2,0) (0,2)
    o.uv   = c;
    o.clip = float4(lerp(gOverlayRect.xy, gOverlayRect.zw, c), 0.0, 1.0);
    return o;
}

float4 PSOverlay(OverlayOut i) : SV_Target
{
    if (any(i.uv < 0.0) || any(i.uv > 1.0)) discard;           // the quad is a big triangle
    const float a = gOverlayTex.Sample(gOverlaySmp, i.uv).a;
    // Dark panel under white text, so it reads over sky as well as over rock.
    const float3 col = lerp(float3(0.04, 0.05, 0.07), float3(1.0, 1.0, 1.0), a);
    return float4(col, max(a, 0.62));
}

// -----------------------------------------------------------------------------
// Tessellation: VS -> HS -> DS -> PS, adding micro-relief close to the camera.
//
// Crack-free by construction. A tessellation factor is computed from the edge's
// own two endpoints, so the two triangles sharing an edge always arrive at the
// same number and fractional_odd partitioning then splits it identically from
// both sides -- no negotiation between patches, and none between chunks either,
// since the endpoints are the same world positions whoever asks.
//
// The displacement is a pure function of world position for the same reason. It
// fades to zero over a band before it stops being applied, so the join against
// untessellated geometry further out has nothing to crack along.
//
// Simplified against shaders/Displacement.hlsli, which is the real definition
// and wants the material table and triplanar height maps this classic path does
// not carry. The structure is the same: relief per material, faded by distance.
// -----------------------------------------------------------------------------
struct CtrlPoint {
    float3 camRel : TEXCOORD0;
    float3 nrm    : TEXCOORD1;
    float3 albedo : TEXCOORD2;
    float  ao     : TEXCOORD3;
    float3 world  : TEXCOORD4;
};

CtrlPoint VSTess(VSIn i)
{
    CtrlPoint o;
    o.camRel = i.pos + gChunkOffset;
    o.world  = i.pos + gChunkWorld;
    o.nrm    = i.nrm;
    const uint  primary   = i.mat & 0xFFu;
    const uint  secondary = (i.mat >> 8) & 0xFFu;
    const float blend     = float((i.mat >> 16) & 0xFFu) * (1.0 / 255.0);
    o.ao                  = float((i.mat >> 24) & 0xFFu) * (1.0 / 255.0);
    float3 albedo = lerp(gMaterial[min(primary, 7u)].rgb, gMaterial[min(secondary, 7u)].rgb, blend);
    o.albedo = lerp(albedo, kLodTint[min(uint(gChunkLod), 6u)], gLodTint);
    return o;
}

float dispFadeAt(float dist)
{
    return 1.0 - saturate((dist - gDispFar) / max(gDispFade, 1e-3));
}

// Micro-relief in metres, before amplitude.
//
// Three octaves with a ridged middle one, so the result is cragged rather than
// a field of smooth lumps: smoothing alone just trades flat facets for soft
// ones, and rock up close wants creases and broken edges.
//
// Weighted hard toward steep faces. That is where the silhouette is -- the
// surface is edge-on there, so displacement along the normal moves the outline
// itself, which is the only thing that stops a ridge reading as polygonal --
// and it is not ground anyone has to walk on. Flat ground keeps a quarter of
// it so it does not look like a different material.
float microRelief(float3 wp, float3 n)
{
    const float h = valueNoise(wp * 0.45) - 0.5;
    const float r = 0.5 - abs(valueNoise(wp * 1.3) - 0.5) * 2.0;    // ridged: creases, not bumps
    const float d = (valueNoise(wp * 3.1) - 0.5) * 0.35;
    const float relief = h + r * 0.55 + d;
    const float steep = 1.0 - saturate(n.y);
    // Gentle ground gets half what it did: at eye level you are mostly looking
    // at it, and that much relief read as noise rather than as ground. Steep
    // faces keep theirs, since that is the half doing the useful work -- they
    // are the silhouette, and they are not what you are standing on.
    return relief * (0.28 + 1.77 * steep * steep);
}

struct PatchConst { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };

float edgeTess(float3 a, float3 b)
{
    const float3 mid = 0.5 * (a + b);
    const float d = max(length(mid), 0.5);
    if (d > gDispFar + gDispFade) return 1.0;        // nothing to add this far out
    // How long this edge is on screen, against the edge size we are aiming for.
    const float px = length(a - b) * gPixPerRadian / d;
    return clamp(px / max(gTessTargetPx, 1.0), 1.0, gTessMax);
}

PatchConst HSConst(InputPatch<CtrlPoint, 3> p)
{
    PatchConst o;
    // SV_TessFactor[i] is the edge opposite control point i.
    o.edges[0] = edgeTess(p[1].camRel, p[2].camRel);
    o.edges[1] = edgeTess(p[2].camRel, p[0].camRel);
    o.edges[2] = edgeTess(p[0].camRel, p[1].camRel);
    o.inside = max(o.edges[0], max(o.edges[1], o.edges[2]));
    return o;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HSConst")]
CtrlPoint HSMain(InputPatch<CtrlPoint, 3> p, uint id : SV_OutputControlPointID) { return p[id]; }

[domain("tri")]
VSOut DSMain(PatchConst pc, float3 bary : SV_DomainLocation, const OutputPatch<CtrlPoint, 3> p)
{
    VSOut o;
    float3 camRel = bary.x * p[0].camRel + bary.y * p[1].camRel + bary.z * p[2].camRel;
    float3 nrm    = normalize(bary.x * p[0].nrm + bary.y * p[1].nrm + bary.z * p[2].nrm);
    const float3 world = bary.x * p[0].world + bary.y * p[1].world + bary.z * p[2].world;
    o.albedo = bary.x * p[0].albedo + bary.y * p[1].albedo + bary.z * p[2].albedo;
    o.ao     = bary.x * p[0].ao + bary.y * p[1].ao + bary.z * p[2].ao;

    const float fade = dispFadeAt(length(camRel));
    if (fade > 0.001)
    {
        const float amp = gDispAmp * fade;
        const float h0 = microRelief(world, nrm);
        // Two tangent taps give the relief's own slope, which is what the
        // shading needs: displacing without re-normalling just makes smooth
        // bumps that catch the light as if they were flat.
        const float3 t1 = normalize(abs(nrm.y) < 0.9 ? cross(nrm, float3(0, 1, 0)) : cross(nrm, float3(1, 0, 0)));
        const float3 t2 = cross(nrm, t1);
        const float e = 0.22;        // tighter than the finest octave, or the normal misses it
        const float h1 = microRelief(world + t1 * e, nrm);
        const float h2 = microRelief(world + t2 * e, nrm);
        camRel += nrm * (h0 * amp);
        nrm = normalize(nrm - (t1 * (h1 - h0) + t2 * (h2 - h0)) * (amp / e));
    }
    o.camRel = camRel;
    o.nrm    = nrm;
    o.clip   = mul(float4(camRel, 1.0), gViewProj);
    return o;
}

// -----------------------------------------------------------------------------
// Far field. Beyond the meshed shell the terrain is never turned into triangles:
// the streamer hands those chunks over as density bricks and they are ray
// marched here instead. Same generator, same DeltaStore, same samples -- only
// the consumer differs, which is what lets the two halves meet without a seam.
//
// The march is two-level. A DDA walks the chunk grid, where a chunk is 256 m or
// more, so empty sky and deep rock are crossed in one step each; only chunks
// holding a surface have a brick allocated, and only those are stepped through
// finely. Inside one, density is close enough to metres to sphere trace on.
// -----------------------------------------------------------------------------
cbuffer Far : register(b2)
{
    float3 gCamWorld;   float gChunkSizeM;    // metres along a traced chunk edge
    float3 gWorldMin;   float gVoxelSizeM;    // metres between traced samples
    float3 gIndexDims;  float gFarStart;      // index volume extent, in chunks
    float3 gAtlasDims;  float gFarEnd;        // brick atlas extent, in texels
    float3 gSlotGrid;   float gFarPad;        // brick slots per atlas axis
};
Texture3D<uint>   gIndex  : register(t0);
Texture3D<float2> gAtlas  : register(t1);   // r = density, g = material index
SamplerState      gLinear : register(s0);

static const float kBrick = 34.0;       // samples per brick edge
static const float kSpan  = 31.999;     // the chunk's own 32 cells, kept off the far border

float3 slotOrigin(uint slot)
{
    const uint gx = uint(gSlotGrid.x), gy = uint(gSlotGrid.y);
    return float3(float(slot % gx), float((slot / gx) % gy), float(slot / (gx * gy))) * kBrick;
}

// Brick sample i holds the chunk's local point i-1, so the +1 skips the border.
float sampleBrick(uint slot, float3 local)
{
    const float3 texel = slotOrigin(slot) + local + 1.0;
    const float s = gAtlas.SampleLevel(gLinear, (texel + 0.5) / gAtlasDims, 0).r;
    return (s - 0.5) * 16.0;            // unorm8 over [-8, +8] metres
}

// Material is a palette index, so it is point loaded: interpolating between
// grass and rock would invent a material that is neither.
uint materialAt(uint slot, float3 local)
{
    const int3 texel = int3(slotOrigin(slot) + round(local) + 1.0);
    return uint(gAtlas.Load(int4(texel, 0)).g * 255.0 + 0.5);
}

float3 brickNormal(uint slot, float3 chunkOrigin, float3 p)
{
    const float3 l = (p - chunkOrigin) / gVoxelSizeM;
    const float3 e = float3(0.6, 0.0, 0.0);
    const float dx = sampleBrick(slot, clamp(l + e.xyy, 0.0, kSpan)) - sampleBrick(slot, clamp(l - e.xyy, 0.0, kSpan));
    const float dy = sampleBrick(slot, clamp(l + e.yxy, 0.0, kSpan)) - sampleBrick(slot, clamp(l - e.yxy, 0.0, kSpan));
    const float dz = sampleBrick(slot, clamp(l + e.yyx, 0.0, kSpan)) - sampleBrick(slot, clamp(l - e.yyx, 0.0, kSpan));
    // Density rises toward air, so its gradient already points out of the rock.
    return normalize(float3(dx, dy, dz) + float3(0.0, 1e-5, 0.0));
}

bool traceFar(float3 rd, out float3 outHit, out float3 outNrm, out uint outMat)
{
    outHit = 0.0;
    outNrm = float3(0, 1, 0);
    outMat = 3;                                  // rock, if nothing else says otherwise
    const float3 ro = gCamWorld;
    const float3 ip = (ro - gWorldMin) / gChunkSizeM;     // ray in chunk units
    const float3 id = rd / gChunkSizeM;

    float t = gFarStart;
    int3 cell = int3(floor(ip + id * t));
    const int3 stepDir = int3(id.x >= 0 ? 1 : -1, id.y >= 0 ? 1 : -1, id.z >= 0 ? 1 : -1);
    const float3 dt = 1.0 / max(abs(id), 1e-20);          // t spent crossing one chunk
    float3 next;
    [unroll] for (int k = 0; k < 3; ++k) {
        const float cur = ip[k] + id[k] * t;
        const float bound = float(cell[k]) + (stepDir[k] > 0 ? 1.0 : 0.0);
        next[k] = abs(id[k]) < 1e-20 ? 1e30 : t + (bound - cur) / id[k];
    }

    [loop] for (int iter = 0; iter < 160 && t < gFarEnd; ++iter) {
        if (all(cell >= int3(0, 0, 0)) && all(cell < int3(gIndexDims))) {
            const uint slot = gIndex.Load(int4(cell, 0));
            if (slot != 0xFFFFFFFFu) {
                const float tExit = min(min(next.x, min(next.y, next.z)), gFarEnd);
                const float3 chunkOrigin = gWorldMin + float3(cell) * gChunkSizeM;
                float s = t;
                [loop] for (int j = 0; j < 96 && s < tExit; ++j) {
                    const float3 l = clamp((ro + rd * s - chunkOrigin) / gVoxelSizeM, 0.0, kSpan);
                    const float d = sampleBrick(slot, l);
                    if (d < 0.0) {
                        float lo = max(s - gVoxelSizeM, t), hi = s;
                        [unroll] for (int b = 0; b < 6; ++b) {
                            const float mid = 0.5 * (lo + hi);
                            const float3 lm = clamp((ro + rd * mid - chunkOrigin) / gVoxelSizeM, 0.0, kSpan);
                            if (sampleBrick(slot, lm) < 0.0) hi = mid; else lo = mid;
                        }
                        outHit = ro + rd * hi;
                        outNrm = brickNormal(slot, chunkOrigin, outHit);
                        // Read the material a full voxel inside the surface. On
                        // it, or just under it, the nearest sample is as likely
                        // to be the air above -- which reads as a speckle of
                        // the wrong colour all over anything steep.
                        const float3 inside = outHit - outNrm * gVoxelSizeM;
                        outMat = materialAt(slot, clamp((inside - chunkOrigin) / gVoxelSizeM, 0.0, kSpan));
                        if (outMat == 0u) outMat = 3u;   // air: the tap missed, call it rock
                        return true;
                    }
                    // Density is near enough to a distance to step on, floored so
                    // a grazing ray cannot stall inside one voxel. The floor
                    // also opens with distance: a step finer than the pixel the
                    // ray covers buys nothing, and a ray skimming a ridge for a
                    // kilometre is where the whole march budget goes.
                    s += max(max(d * 0.8, gVoxelSizeM * 0.35), s * 0.0045);
                }
            }
        }
        if (next.x < next.y && next.x < next.z)      { t = next.x; cell.x += stepDir.x; next.x += dt.x; }
        else if (next.y < next.z)                    { t = next.y; cell.y += stepDir.y; next.y += dt.y; }
        else                                         { t = next.z; cell.z += stepDir.z; next.z += dt.z; }

        // Out of the volume and heading further out: nothing can be hit from
        // here. Without this a ray into the sky leaves the 8-cell-tall index
        // after a step or two and then spends its whole iteration budget
        // walking empty space, which is most of the pixels in a level view.
        const int3 dim = int3(gIndexDims);
        if ((cell.x < 0 && stepDir.x < 0) || (cell.x >= dim.x && stepDir.x > 0) ||
            (cell.y < 0 && stepDir.y < 0) || (cell.y >= dim.y && stepDir.y > 0) ||
            (cell.z < 0 && stepDir.z < 0) || (cell.z >= dim.z && stepDir.z > 0)) break;
    }
    return false;
}

float4 PSFar(SkyOut i) : SV_Target
{
    const float3 dir = normalize(gCamFwd + gCamRight * (i.ndc.x * gTanHalfFovX) +
                                           gCamUp    * (i.ndc.y * gTanHalfFovY));
    float3 hit, n;
    uint mat;
    // Returning the sky rather than discarding, deliberately. A shader that can
    // discard cannot be depth-rejected before it runs, so `discard` here made
    // every pixel on screen march -- including the ~70% already covered by the
    // meshed near field, whose depth would otherwise have killed them for free.
    // This pass only runs where the depth test passes, which is where the sky
    // is, so writing the sky back costs nothing and buys early-Z.
    if (!traceFar(dir, hit, n, mat))
        return float4(pow(saturate(skyColor(dir)), 1.0 / 2.2), 1.0);

    const float3 camRel = hit - gCamWorld;
    const float  dist   = length(camRel);
    const float3 v      = -dir;

    // The same palette the meshed path indexes, so the two halves of the
    // landscape are not merely the same shape but the same colour.
    float3 albedo = lerp(gMaterial[min(mat, 7u)].rgb, kLodTint[6], gLodTint);
    const float  ndl = saturate(dot(n, gSunDir));
    const float3 sun = gSunColor * ndl;
    const float3 amb = lerp(float3(0.10, 0.10, 0.12), gSkyTop * 0.55, saturate(n.y * 0.5 + 0.5));
    const float3 h   = normalize(gSunDir + v);
    const float  spec = pow(saturate(dot(n, h)), 24.0) * 0.06 * ndl;
    float3 color = albedo * (sun + amb) + spec;

    // Identical fog and tonemap to the meshed path, or the handover would show
    // as a change in haze rather than a change in geometry.
    const float heightFalloff = clamp(exp(-camRel.y * 0.0012), 0.55, 1.8);
    const float fog = 1.0 - exp(-dist * gFogDensity * heightFalloff);
    color = lerp(color, skyColor(dir), saturate(fog));
    color = (color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14);
    return float4(pow(saturate(color), 1.0 / 2.2), 1.0);
}
)HLSL";

// -----------------------------------------------------------------------------
// Camera
// -----------------------------------------------------------------------------
struct Camera {
    Vec3d position{};
    float yaw = 0.f;     // radians, 0 looks down +Z
    float pitch = 0.f;

    Vec3f forward() const {
        const float cp = std::cos(pitch);
        return {cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw)};
    }
    Vec3f right() const {
        const Vec3f f = forward();
        return normalize(Vec3f{f.z, 0.f, -f.x});   // cross(worldUp, forward), left-handed
    }
    Vec3f up() const { return cross(forward(), right()); }

    void lookAt(const Vec3d& target) {
        const Vec3d d = target - position;
        const double horiz = std::sqrt(d.x * d.x + d.z * d.z);
        yaw = float(std::atan2(d.x, d.z));
        pitch = float(std::atan2(d.y, std::max(horiz, 1e-6)));
    }
};

// Shortest-arc interpolation so the flyby never spins the long way round.
float lerpAngle(float a, float b, float t) {
    float d = std::fmod(b - a + 3.14159265f * 3.f, 6.2831853f) - 3.14159265f;
    return a + d * t;
}

Vec3d catmullRom(const Vec3d& p0, const Vec3d& p1, const Vec3d& p2, const Vec3d& p3, double t) {
    const double t2 = t * t, t3 = t2 * t;
    return (p1 * 2.0 + (p2 - p0) * t + (p0 * 2.0 - p1 * 5.0 + p2 * 4.0 - p3) * t2 +
            (p1 * 3.0 - p0 - p2 * 3.0 + p3) * t3) * 0.5;
}

// Closed Catmull-Rom loop through `keys`, re-parameterised by arc length so the
// camera moves at a constant speed regardless of how the keys are spaced.
class SplinePath {
public:
    void build(std::vector<Vec3d> keys, int samplesPerSegment = 64) {
        m_keys = std::move(keys);
        m_arc.clear();
        m_total = 0.0;
        if (m_keys.size() < 4) return;
        const int n = int(m_keys.size());
        m_arc.reserve(size_t(n * samplesPerSegment) + 1);
        m_arc.push_back(0.0);
        Vec3d prev = evalRaw(0.0);
        for (int s = 1; s <= n * samplesPerSegment; ++s) {
            const Vec3d cur = evalRaw(double(s) / double(samplesPerSegment));
            m_total += length(cur - prev);
            m_arc.push_back(m_total);
            prev = cur;
        }
        m_samplesPerSegment = samplesPerSegment;
    }

    bool valid() const { return m_keys.size() >= 4 && m_total > 0.0; }
    double totalLength() const { return m_total; }

    // distance in metres along the loop -> position
    Vec3d atDistance(double distance) const {
        if (!valid()) return m_keys.empty() ? Vec3d{} : m_keys[0];
        distance = std::fmod(distance, m_total);
        if (distance < 0.0) distance += m_total;
        const auto it = std::upper_bound(m_arc.begin(), m_arc.end(), distance);
        const size_t hi = size_t(std::min<ptrdiff_t>(it - m_arc.begin(), ptrdiff_t(m_arc.size()) - 1));
        const size_t lo = hi > 0 ? hi - 1 : 0;
        const double span = m_arc[hi] - m_arc[lo];
        const double frac = span > 1e-9 ? (distance - m_arc[lo]) / span : 0.0;
        return evalRaw((double(lo) + frac) / double(m_samplesPerSegment));
    }

    // Arc-length position of the sample closest to `p`. Used when re-engaging the
    // flyby from free-cam so the camera rejoins the loop where it is standing
    // instead of snapping back to wherever the cinematic had got to.
    double nearestDistance(const Vec3d& p) const {
        if (!valid()) return 0.0;
        double best = 0.0, bestD2 = 1e300;
        for (size_t s = 0; s + 1 < m_arc.size(); ++s) {
            const Vec3d q = evalRaw(double(s) / double(m_samplesPerSegment));
            const Vec3d d = q - p;
            const double d2 = dot(d, d);
            if (d2 < bestD2) { bestD2 = d2; best = m_arc[s]; }
        }
        return best;
    }

private:
    // u is in "segments": [0, keyCount) wraps around the loop.
    Vec3d evalRaw(double u) const {
        const int n = int(m_keys.size());
        const double wrapped = std::fmod(u, double(n)) + (u < 0 ? double(n) : 0.0);
        const int i = int(std::floor(wrapped));
        const double t = wrapped - double(i);
        auto at = [&](int k) { return m_keys[size_t(((k % n) + n) % n)]; };
        return catmullRom(at(i - 1), at(i), at(i + 1), at(i + 2), t);
    }

    std::vector<Vec3d> m_keys;
    std::vector<double> m_arc;
    double m_total = 0.0;
    int m_samplesPerSegment = 64;
};

// -----------------------------------------------------------------------------
// D3D11 chunk registry: an IChunkListener that turns OptimizedChunkMesh into
// per-chunk vertex/index buffers. Uploads are budgeted per frame so a burst of
// completed chunks never stalls the present.
// -----------------------------------------------------------------------------
class ChunkGpuRegistry final : public IChunkListener {
public:
    struct Entry {
        Com<ID3D11Buffer> vb, ib;
        UINT indexCount = 0;
        uint32_t version = 0;
        Vec3d origin{};
        Aabbf localBounds{};
        uint8_t lod = 0;
    };

    explicit ChunkGpuRegistry(ID3D11Device* device) : m_device(device) {}

    // --- IChunkListener (main thread, inside ChunkStreamer::update) ---
    void onChunkMeshReady(const ChunkKey& key, uint32_t version,
                          const std::shared_ptr<const OptimizedChunkMesh>& mesh) override {
        if (!mesh || mesh->empty()) { m_pendingRemoval.push_back(key); return; }
        m_latest[key] = version;
        m_pending.push_back({key, version, mesh});
    }
    void onChunkRemoved(const ChunkKey& key) override {
        m_latest.erase(key);
        m_pendingRemoval.push_back(key);
    }

    // Creates at most `budget` buffers this frame; returns how many it made.
    uint32_t flushUploads(uint32_t budget) {
        for (const ChunkKey& k : m_pendingRemoval) m_chunks.erase(k);
        m_pendingRemoval.clear();

        uint32_t made = 0;
        while (made < budget && !m_pending.empty()) {
            Upload up = std::move(m_pending.front());
            m_pending.pop_front();
            const auto latest = m_latest.find(up.key);
            if (latest == m_latest.end() || latest->second != up.version) continue;   // superseded
            if (create(up)) ++made;
        }
        return made;
    }

    size_t pendingUploads() const { return m_pending.size(); }
    const std::unordered_map<ChunkKey, Entry, ChunkKeyHash>& chunks() const { return m_chunks; }

private:
    struct Upload {
        ChunkKey key;
        uint32_t version;
        std::shared_ptr<const OptimizedChunkMesh> mesh;
    };

    bool create(const Upload& up) {
        const OptimizedChunkMesh& m = *up.mesh;
        static_assert(sizeof(TerrainVertex) == 28, "vertex layout must match the input elements below");

        Entry e;
        e.version = up.version;
        e.origin = up.key.originWorld();
        e.localBounds = m.bounds;
        e.lod = up.key.lod;
        e.indexCount = UINT(m.indices.size());

        D3D11_BUFFER_DESC bd{};
        D3D11_SUBRESOURCE_DATA sd{};

        bd.ByteWidth = UINT(m.vertices.size() * sizeof(TerrainVertex));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        sd.pSysMem = m.vertices.data();
        if (fail(m_device->CreateBuffer(&bd, &sd, e.vb.put()), "CreateBuffer(vertices)")) return false;

        bd.ByteWidth = UINT(m.indices.size() * sizeof(uint32_t));
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        sd.pSysMem = m.indices.data();
        if (fail(m_device->CreateBuffer(&bd, &sd, e.ib.put()), "CreateBuffer(indices)")) return false;

        m_chunks[up.key] = std::move(e);
        return true;
    }

    ID3D11Device* m_device;
    std::unordered_map<ChunkKey, Entry, ChunkKeyHash> m_chunks;
    std::unordered_map<ChunkKey, uint32_t, ChunkKeyHash> m_latest;
    std::deque<Upload> m_pending;
    std::vector<ChunkKey> m_pendingRemoval;
};

// -----------------------------------------------------------------------------
// TextOverlay: the controls, on screen.
//
// GDI draws the text into a DIB once and that becomes a texture, which is a lot
// less code than a hand-rolled bitmap font and reads better. GDI does not write
// an alpha channel though -- it leaves it at whatever was there -- so coverage
// is recovered from the luminance it did write, which works because the text is
// drawn white on a cleared buffer.
// -----------------------------------------------------------------------------
class TextOverlay {
public:
    explicit TextOverlay(ID3D11Device* device) : m_device(device) {}

    // Rebuilds the texture. Cheap enough to call whenever the text changes.
    bool build(const std::string& text, int width, int lineHeight) {
        int lines = 1;
        for (char c : text) if (c == '\n') ++lines;
        const int height = lines * lineHeight + 14;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;          // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) { DeleteDC(dc); return false; }
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        std::memset(bits, 0, size_t(width) * size_t(height) * 4);

        HFONT font = CreateFontA(-(lineHeight - 4), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 FIXED_PITCH | FF_MODERN, "Consolas");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT r{10, 7, width - 8, height - 4};
        DrawTextA(dc, text.c_str(), -1, &r, DT_LEFT | DT_TOP | DT_NOCLIP | DT_EXPANDTABS);
        GdiFlush();

        // GDI wrote colour but no alpha; the brightness it wrote is the glyph
        // coverage, so lift it into alpha and leave the text white.
        auto* px = static_cast<uint8_t*>(bits);
        for (size_t i = 0, n = size_t(width) * size_t(height); i < n; ++i) {
            const uint8_t a = std::max({px[i * 4 + 0], px[i * 4 + 1], px[i * 4 + 2]});
            px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = 255;
            px[i * 4 + 3] = a;
        }

        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(width); td.Height = UINT(height);
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = bits;
        init.SysMemPitch = UINT(width) * 4;

        m_srv.reset();
        m_tex.reset();
        const bool ok = SUCCEEDED(m_device->CreateTexture2D(&td, &init, m_tex.put())) &&
                        SUCCEEDED(m_device->CreateShaderResourceView(m_tex.get(), nullptr, m_srv.put()));
        m_width = width;
        m_height = height;

        SelectObject(dc, oldFont);
        DeleteObject(font);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        return ok;
    }

    ID3D11ShaderResourceView* srv() const { return m_srv.get(); }
    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    ID3D11Device* m_device;
    Com<ID3D11Texture2D> m_tex;
    Com<ID3D11ShaderResourceView> m_srv;
    int m_width = 0, m_height = 0;
};

// -----------------------------------------------------------------------------
// Far-field brick store. Chunks at or beyond StreamerConfig::traceLod arrive
// here as density instead of geometry. They live in a GPU brick atlas indexed
// by a world-anchored volume of slot ids, which is what the trace shader walks:
// one texel per chunk of the world at that LOD, holding either a slot or
// "nothing here". Slots recycle as chunks come and go, so the atlas is sized
// for peak residency rather than for the world.
//
// Only chunks with a surface in them ever get a slot -- open sky and deep rock
// reduce to a single empty texel, which is what makes the ray able to cross a
// kilometre of nothing in a couple of steps.
// -----------------------------------------------------------------------------
class FarFieldBricks final : public IChunkListener {
public:
    static constexpr int kSlotsX = 16, kSlotsY = 16, kSlotsZ = 8;      // 2048 bricks
    static constexpr uint32_t kSlotCount = uint32_t(kSlotsX) * kSlotsY * kSlotsZ;
    static constexpr uint32_t kEmpty = 0xFFFFFFFFu;

    FarFieldBricks(ID3D11Device* device, uint8_t traceLod) : m_device(device), m_lod(traceLod) {}

    bool init() {
        const int32_t cs = chunkSizeVoxels();
        m_base = {floorDiv(-world::kWorldHalfExtentXZ, cs), floorDiv(world::kWorldMinY, cs),
                  floorDiv(-world::kWorldHalfExtentXZ, cs)};
        m_dim = {floorDiv(world::kWorldHalfExtentXZ, cs) - m_base.x + 1,
                 floorDiv(world::kWorldMaxY, cs) - m_base.y + 1,
                 floorDiv(world::kWorldHalfExtentXZ, cs) - m_base.z + 1};

        m_index.assign(size_t(m_dim.x) * size_t(m_dim.y) * size_t(m_dim.z), kEmpty);
        for (uint32_t i = 0; i < kSlotCount; ++i) m_free.push_back(kSlotCount - 1 - i);

        D3D11_TEXTURE3D_DESC id{};
        id.Width = UINT(m_dim.x); id.Height = UINT(m_dim.y); id.Depth = UINT(m_dim.z);
        id.MipLevels = 1;
        id.Format = DXGI_FORMAT_R32_UINT;
        id.Usage = D3D11_USAGE_DEFAULT;
        id.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        // Must start as "nothing here" everywhere: a texture created without
        // initial data holds garbage, and a garbage slot id sends the ray into
        // an arbitrary brick.
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = m_index.data();
        init.SysMemPitch = UINT(sizeof(uint32_t) * size_t(m_dim.x));
        init.SysMemSlicePitch = init.SysMemPitch * UINT(m_dim.y);
        if (FAILED(m_device->CreateTexture3D(&id, &init, m_indexTex.put()))) return false;
        if (FAILED(m_device->CreateShaderResourceView(m_indexTex.get(), nullptr, m_indexSrv.put()))) return false;

        D3D11_TEXTURE3D_DESC ad{};
        ad.Width = UINT(kSlotsX * kBrickSize); ad.Height = UINT(kSlotsY * kBrickSize);
        ad.Depth = UINT(kSlotsZ * kBrickSize);
        ad.MipLevels = 1;
        ad.Format = DXGI_FORMAT_R8G8_UNORM;   // r = density, g = material index
        ad.Usage = D3D11_USAGE_DEFAULT;
        ad.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m_device->CreateTexture3D(&ad, nullptr, m_atlasTex.put()))) return false;
        if (FAILED(m_device->CreateShaderResourceView(m_atlasTex.get(), nullptr, m_atlasSrv.put()))) return false;

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        return SUCCEEDED(m_device->CreateSamplerState(&sd, m_sampler.put()));
    }

    // --- IChunkListener -----------------------------------------------------
    void onChunkMeshReady(const ChunkKey&, uint32_t, const std::shared_ptr<const OptimizedChunkMesh>&) override {}

    void onChunkDensityReady(const ChunkKey& key, uint32_t version,
                             const std::shared_ptr<const DensityBrick>& brick) override {
        // One index volume, so one chunk size: a coarser brick would land in
        // the wrong cell rather than simply miss. Levels above traceLod are
        // dropped until the index becomes a per-level clipmap, which is why
        // the demo wants --maxlod equal to --trace-lod.
        if (key.lod != m_lod) return;
        if (!brick) { m_pendingRemoval.push_back(key); return; }   // uniform: nothing to trace
        m_latest[key] = version;
        m_pending.push_back({key, version, brick});
    }
    void onChunkRemoved(const ChunkKey& key) override {
        m_latest.erase(key);
        m_pendingRemoval.push_back(key);
    }

    uint32_t flushUploads(ID3D11DeviceContext* ctx, uint32_t budget) {
        for (const ChunkKey& k : m_pendingRemoval) release(ctx, k);
        m_pendingRemoval.clear();

        uint32_t made = 0;
        while (made < budget && !m_pending.empty()) {
            Upload up = std::move(m_pending.front());
            m_pending.pop_front();
            const auto latest = m_latest.find(up.key);
            if (latest == m_latest.end() || latest->second != up.version) continue;   // superseded
            if (upload(ctx, up)) { ++made; ++m_totalUploads; }
        }
        return made;
    }

    uint64_t totalUploads() const { return m_totalUploads; }

    void bind(ID3D11DeviceContext* ctx) const {
        ID3D11ShaderResourceView* srvs[2] = {m_indexSrv.get(), m_atlasSrv.get()};
        ctx->PSSetShaderResources(0, 2, srvs);
        ID3D11SamplerState* samplers[1] = {m_sampler.get()};
        ctx->PSSetSamplers(0, 1, samplers);
    }
    static void unbind(ID3D11DeviceContext* ctx) {
        ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
        ctx->PSSetShaderResources(0, 2, none);
    }

    size_t resident() const { return m_slotOf.size(); }
    size_t pendingUploads() const { return m_pending.size(); }
    size_t atlasBytes() const {
        return size_t(kSlotsX * kBrickSize) * size_t(kSlotsY * kBrickSize) * size_t(kSlotsZ * kBrickSize);
    }
    size_t usedBytes() const { return m_slotOf.size() * size_t(kBrickVolume); }
    bool overflowed() const { return m_overflow; }
    Vec3i indexDim() const { return m_dim; }
    Vec3d indexOriginWorld() const {
        const double cs = double(chunkSizeVoxels()) * world::kVoxelSize;
        return {double(m_base.x) * cs, double(m_base.y) * cs, double(m_base.z) * cs};
    }
    int32_t chunkSizeVoxels() const { return world::kChunkCells << m_lod; }

private:
    struct Upload {
        ChunkKey key;
        uint32_t version;
        std::shared_ptr<const DensityBrick> brick;
    };

    bool inRange(const Vec3i& c) const {
        return c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < m_dim.x && c.y < m_dim.y && c.z < m_dim.z;
    }
    Vec3i cellOf(const ChunkKey& key) const { return {key.x - m_base.x, key.y - m_base.y, key.z - m_base.z}; }
    size_t flat(const Vec3i& c) const { return size_t(c.x) + size_t(m_dim.x) * (size_t(c.y) + size_t(m_dim.y) * size_t(c.z)); }

    void writeIndex(ID3D11DeviceContext* ctx, const Vec3i& c, uint32_t value) {
        m_index[flat(c)] = value;
        const D3D11_BOX box{UINT(c.x), UINT(c.y), UINT(c.z), UINT(c.x) + 1, UINT(c.y) + 1, UINT(c.z) + 1};
        ctx->UpdateSubresource(m_indexTex.get(), 0, &box, &value, sizeof(uint32_t), sizeof(uint32_t));
    }

    bool upload(ID3D11DeviceContext* ctx, const Upload& up) {
        const Vec3i cell = cellOf(up.key);
        if (!inRange(cell)) return false;

        auto it = m_slotOf.find(up.key);
        uint32_t slot;
        if (it != m_slotOf.end()) {
            slot = it->second;                       // rebuilt in place after an edit
        } else {
            if (m_free.empty()) { m_overflow = true; return false; }
            slot = m_free.back();
            m_free.pop_back();
            m_slotOf.emplace(up.key, slot);
        }

        const UINT sx = UINT((slot % kSlotsX) * kBrickSize);
        const UINT sy = UINT(((slot / kSlotsX) % kSlotsY) * kBrickSize);
        const UINT sz = UINT((slot / (kSlotsX * kSlotsY)) * kBrickSize);
        const D3D11_BOX box{sx, sy, sz, sx + UINT(kBrickSize), sy + UINT(kBrickSize), sz + UINT(kBrickSize)};
        ctx->UpdateSubresource(m_atlasTex.get(), 0, &box, up.brick->texels.data(), UINT(kBrickSize) * 2,
                               UINT(kBrickSize * kBrickSize) * 2);
        writeIndex(ctx, cell, slot);
        return true;
    }

    void release(ID3D11DeviceContext* ctx, const ChunkKey& key) {
        auto it = m_slotOf.find(key);
        if (it == m_slotOf.end()) return;
        const Vec3i cell = cellOf(key);
        if (inRange(cell)) writeIndex(ctx, cell, kEmpty);
        m_free.push_back(it->second);
        m_slotOf.erase(it);
    }

    ID3D11Device* m_device;
    uint8_t m_lod;
    Vec3i m_base{}, m_dim{};
    std::vector<uint32_t> m_index;
    std::vector<uint32_t> m_free;
    std::unordered_map<ChunkKey, uint32_t, ChunkKeyHash> m_slotOf;
    std::unordered_map<ChunkKey, uint32_t, ChunkKeyHash> m_latest;
    std::deque<Upload> m_pending;
    std::vector<ChunkKey> m_pendingRemoval;
    Com<ID3D11Texture3D> m_indexTex, m_atlasTex;
    Com<ID3D11ShaderResourceView> m_indexSrv, m_atlasSrv;
    Com<ID3D11SamplerState> m_sampler;
    uint64_t m_totalUploads = 0;
    bool m_overflow = false;
};

// -----------------------------------------------------------------------------
// Constant buffer layouts (must match the HLSL above)
// -----------------------------------------------------------------------------
struct FrameCB {
    float viewProj[16];
    float sunDir[3];     float fogDensity;
    float sunColor[3];   float lodTint;
    float skyTop[3];     float tanHalfFovX;
    float skyHorizon[3]; float tanHalfFovY;
    float camRight[3];   float pad0;
    float camUp[3];      float pad1;
    float camFwd[3];     float pad2;
    float material[8][4];
    float tessTargetPx;  float pixPerRadian;
    float dispFar;       float dispFade;
    float dispAmp;       float tessMax;
    float pad3;          float pad4;
};
static_assert(sizeof(FrameCB) % 16 == 0);

struct ChunkCB {
    float offset[3];
    float lod;
    float world[3];
    float pad;
};
static_assert(sizeof(ChunkCB) % 16 == 0);

struct OverlayCB { float rect[4]; };
static_assert(sizeof(OverlayCB) % 16 == 0);

struct FarCB {
    float camWorld[3];  float chunkSizeM;
    float worldMin[3];  float voxelSizeM;
    float indexDims[3]; float farStart;
    float atlasDims[3]; float farEnd;
    float slotGrid[3];  float pad;
};
static_assert(sizeof(FarCB) % 16 == 0);

// -----------------------------------------------------------------------------
// Input state
// -----------------------------------------------------------------------------
struct Input {
    bool keys[256]{};
    bool pressed[256]{};      // edge-triggered, consumed once per frame
    int mouseDx = 0, mouseDy = 0;
    int wheel = 0;
    bool dig = false, fill = false;
    bool captured = false;
    bool resized = false;
    bool quit = false;
    UINT width = 1600, height = 900;

    bool takePress(int vk) {
        const bool v = pressed[vk & 0xFF];
        pressed[vk & 0xFF] = false;
        return v;
    }
};

Input g_input;
HWND g_hwnd = nullptr;

void setCapture(bool on) {
    if (g_input.captured == on) return;
    g_input.captured = on;
    if (on) {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        POINT tl{rc.left, rc.top}, br{rc.right, rc.bottom};
        ClientToScreen(g_hwnd, &tl);
        ClientToScreen(g_hwnd, &br);
        const RECT clip{tl.x, tl.y, br.x, br.y};
        ClipCursor(&clip);
        SetCursorPos((tl.x + br.x) / 2, (tl.y + br.y) / 2);
        while (ShowCursor(FALSE) >= 0) {}
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
    }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_DESTROY:
    case WM_CLOSE:
        g_input.quit = true;
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && LOWORD(lp) && HIWORD(lp)) {
            g_input.width = LOWORD(lp);
            g_input.height = HIWORD(lp);
            g_input.resized = true;
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (!(lp & (1 << 30))) g_input.pressed[wp & 0xFF] = true;   // ignore auto-repeat
        g_input.keys[wp & 0xFF] = true;
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        g_input.keys[wp & 0xFF] = false;
        return 0;
    case WM_INPUT: {
        if (!g_input.captured) break;
        RAWINPUT ri{};
        UINT size = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            g_input.mouseDx += ri.data.mouse.lLastX;
            g_input.mouseDy += ri.data.mouse.lLastY;
        }
        break;
    }
    case WM_MOUSEWHEEL:
        g_input.wheel += GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        return 0;
    case WM_LBUTTONDOWN:
        if (g_input.captured) g_input.dig = true; else setCapture(true);
        return 0;
    case WM_RBUTTONDOWN:
        if (g_input.captured) g_input.fill = true;
        return 0;
    case WM_KILLFOCUS:
        setCapture(false);
        std::fill(std::begin(g_input.keys), std::end(g_input.keys), false);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Copies the back buffer to a staging texture and writes it out as a PNG via
// WIC. Used by --screenshot, which makes the viewer verifiable from a script.
bool saveScreenshot(ID3D11Device* device, ID3D11DeviceContext* ctx, IDXGISwapChain1* swapChain, const char* path) {
    Com<ID3D11Texture2D> back;
    if (fail(swapChain->GetBuffer(0, IID_PPV_ARGS(back.put())), "GetBuffer(screenshot)")) return false;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    Com<ID3D11Texture2D> staging;
    if (fail(device->CreateTexture2D(&sd, nullptr, staging.put()), "CreateTexture2D(staging)")) return false;
    ctx->CopyResource(staging.get(), back.get());

    D3D11_MAPPED_SUBRESOURCE map{};
    if (fail(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &map), "Map(staging)")) return false;

    // Pack to tight rows as BGRA, which is what the WIC PNG encoder actually
    // wants: asking it for 32bppRGBA succeeds but silently rewrites the format
    // to its native BGRA, after which the same bytes come out red/blue swapped.
    // Alpha is forced opaque - FLIP_DISCARD leaves the swap chain's alpha
    // channel undefined.
    const bool backBufferIsBgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                                  desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    std::vector<uint8_t> pixels(size_t(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch;
        uint8_t* dst = pixels.data() + size_t(y) * desc.Width * 4;
        for (UINT x = 0; x < desc.Width; ++x) {
            dst[x * 4 + 0] = backBufferIsBgra ? src[x * 4 + 0] : src[x * 4 + 2];   // B
            dst[x * 4 + 1] = src[x * 4 + 1];                                       // G
            dst[x * 4 + 2] = backBufferIsBgra ? src[x * 4 + 2] : src[x * 4 + 0];   // R
            dst[x * 4 + 3] = 255;
        }
    }
    ctx->Unmap(staging.get(), 0);

    Com<IWICImagingFactory> wic;
    if (fail(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put())),
             "CoCreateInstance(WIC)"))
        return false;

    wchar_t wpath[MAX_PATH] = {};
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
    Com<IWICStream> stream;
    Com<IWICBitmapEncoder> encoder;
    Com<IWICBitmapFrameEncode> frame;
    if (fail(wic->CreateStream(stream.put()), "WIC CreateStream")) return false;
    if (fail(stream->InitializeFromFilename(wpath, GENERIC_WRITE), "WIC InitializeFromFilename")) return false;
    if (fail(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()), "WIC CreateEncoder")) return false;
    if (fail(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache), "WIC encoder Initialize")) return false;
    if (fail(encoder->CreateNewFrame(frame.put(), nullptr), "WIC CreateNewFrame")) return false;
    if (fail(frame->Initialize(nullptr), "WIC frame Initialize")) return false;
    if (fail(frame->SetSize(desc.Width, desc.Height), "WIC SetSize")) return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (fail(frame->SetPixelFormat(&fmt), "WIC SetPixelFormat")) return false;
    if (!IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA))
        std::fprintf(stderr, "[vxe_viewer] WIC substituted a different pixel format; colours may be swapped\n");
    if (fail(frame->WritePixels(desc.Height, desc.Width * 4, UINT(pixels.size()), pixels.data()), "WIC WritePixels"))
        return false;
    if (fail(frame->Commit(), "WIC frame Commit")) return false;
    if (fail(encoder->Commit(), "WIC encoder Commit")) return false;
    std::printf("screenshot : %s (%ux%u)\n", path, desc.Width, desc.Height);
    return true;
}

void printControls() {
    std::printf(
        "\ncontrols\n"
        "--------\n"
        "  F            toggle cinematic flyby / free camera\n"
        "  V            toggle walking: eyes 2 m above the ground, no flying\n"
        "  T            toggle tessellation (only with --tess)\n"
        "  click / Tab  capture the mouse (free look).  Tab or Esc releases it\n"
        "  W A S D      move,  Q / E  down / up\n"
        "  Shift        x8 speed,  Ctrl  x1/5 speed,  mouse wheel  set base speed\n"
        "  H            show / hide the controls on screen\n"
        "  LMB / RMB    dig / fill under the crosshair (live CSG + remesh)\n"
        "  - / =        dig radius smaller / larger\n"
        "  G            wireframe,  L  LOD colour tint,  C  backface culling,  R  respawn\n"
        "  [ / ]        flyby speed down / up\n"
        "  Esc          release the mouse, or quit when it is already free\n\n");
}

} // namespace

int main(int argc, char** argv) {
    // ---- Options -------------------------------------------------------------
    double viewDistance = 2200.0;
    int maxLod = 5;
    int seed = 1337;
    UINT width = 1600, height = 900;
    bool vsync = true;
    std::string shotPath;
    double shotAt = 6.0;            // seconds of flyby before the screenshot
    double exitAfter = 0.0;         // 0 = run until closed
    bool startLodTint = false, startWireframe = false, startFlyby = true;
    float skirtCells = -1.f;        // < 0 = leave the StreamerConfig default alone
    bool farTrace = false;          // mesh the near field, ray march the rest
    // Front to back by default: measured at 1.6x overdraw in hash order
    // against ~1.0x sorted, for a sort of a few hundred entries. --no-sort is
    // kept so the two can still be compared.
    bool sortChunks = true;
    bool pipeStats = false;         // count pixel shader invocations: overdraw
    bool stripes = false;           // gradient-aligned stripe erosion on the heights
    float stripeAmp = 5.f, stripeWidth = 60.f;
    float digRadiusStart = 5.f;     // metres; - / = adjust it at runtime
    bool walk = false;              // first-person: eye height above the ground, no flying
    float eyeHeight = 2.f, walkSpeed = 4.5f;
    bool tess = false;              // hardware tessellation + micro-relief up close
    // 0.8 m peak, but the steep weighting means walkable ground sees about a
    // quarter of that and near-vertical rock the rest -- so the figure that has
    // to stay under step height is the flat-ground one, not this.
    float dispAmp = 0.8f;
    // Reach matters as much as amplitude for silhouettes: a ridge is usually
    // further off than the ground you are standing on, and gets no detail at
    // all once the tessellation factor falls back to 1.
    float dispFar = 150.f, tessTargetPx = 9.f, tessMax = 12.f;
    int traceLod = 3;               // first LOD handed over as density instead of geometry
    bool fixedCam = false;
    double camArgs[5] = {};         // x y z yaw pitch (degrees)
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](double def) { return i + 1 < argc ? std::atof(argv[++i]) : def; };
        if (a == "--view") viewDistance = next(viewDistance);
        else if (a == "--maxlod") maxLod = int(next(maxLod));
        else if (a == "--seed") seed = int(next(seed));
        else if (a == "--width") width = UINT(next(width));
        else if (a == "--height") height = UINT(next(height));
        else if (a == "--novsync") vsync = false;
        else if (a == "--screenshot") shotPath = i + 1 < argc ? argv[++i] : "";
        else if (a == "--screenshot-at") shotAt = next(shotAt);
        else if (a == "--exit-after") exitAfter = next(exitAfter);
        else if (a == "--lod-tint") startLodTint = true;
        else if (a == "--wireframe") startWireframe = true;
        else if (a == "--free") startFlyby = false;
        else if (a == "--no-skirts") skirtCells = 0.f;
        else if (a == "--far-trace") farTrace = true;
        else if (a == "--no-sort") sortChunks = false;
        else if (a == "--stripes") stripes = true;
        else if (a == "--dig-radius") digRadiusStart = float(next(digRadiusStart));
        else if (a == "--walk") { walk = true; startFlyby = false; }
        else if (a == "--eye") eyeHeight = float(next(eyeHeight));
        else if (a == "--walk-speed") walkSpeed = float(next(walkSpeed));
        else if (a == "--tess") tess = true;
        else if (a == "--disp-amp") dispAmp = float(next(dispAmp));
        else if (a == "--disp-far") dispFar = float(next(dispFar));
        else if (a == "--tess-px") tessTargetPx = float(next(tessTargetPx));
        else if (a == "--stripe-amp") stripeAmp = float(next(stripeAmp));
        else if (a == "--stripe-width") stripeWidth = float(next(stripeWidth));
        else if (a == "--stats") pipeStats = true;
        else if (a == "--trace-lod") traceLod = int(next(traceLod));
        else if (a == "--skirt") skirtCells = float(next(4.5));
        else if (a == "--cam") {
            for (double& v : camArgs) v = next(0.0);
            fixedCam = true;
            startFlyby = false;
        }
        else if (a == "--help" || a == "-h") {
            std::printf("usage: vxe_viewer [--view M] [--maxlod N] [--seed N] [--width N] [--height N] [--novsync]\n"
                        "                  [--free] [--cam X Y Z YAW PITCH] [--lod-tint] [--wireframe]\n"
                        "                  [--no-skirts] [--skirt CELLS] [--no-sort] [--stats]\n"
                        "                  [--far-trace] [--trace-lod N]   (far-trace wants --maxlod == --trace-lod)\n"
                        "                  [--screenshot FILE.png] [--screenshot-at SECONDS] [--exit-after SECONDS]\n");
            return 0;
        }
    }
    const bool automated = !shotPath.empty() || exitAfter > 0.0;
    if (automated) vsync = false;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    maxLod = std::clamp(maxLod, 2, int(world::kMaxLod));
    g_input.width = width;
    g_input.height = height;

    std::printf("VXE viewer - interactive terrain demo (Direct3D 11 classic path)\n");
    std::printf("================================================================\n");

    // ---- Window --------------------------------------------------------------
    const HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"VxeViewerWindow";
    RegisterClassExW(&wc);

    RECT rc{0, 0, LONG(width), LONG(height)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"VXE viewer", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hinst, nullptr);
    if (!g_hwnd) { std::fprintf(stderr, "CreateWindow failed\n"); return 1; }
    ShowWindow(g_hwnd, SW_SHOW);

    { // raw mouse input for jitter-free, unaccelerated looking
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x02;
        rid.dwFlags = 0;
        rid.hwndTarget = g_hwnd;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }

    // ---- D3D11 ---------------------------------------------------------------
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> ctx;
    UINT deviceFlags = 0;
#ifndef NDEBUG
    deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags, wanted, 2,
                                   D3D11_SDK_VERSION, device.put(), &got, ctx.put());
    if (FAILED(hr) && (deviceFlags & D3D11_CREATE_DEVICE_DEBUG)) {   // no debug layer installed
        deviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags, wanted, 2, D3D11_SDK_VERSION,
                               device.put(), &got, ctx.put());
    }
    if (fail(hr, "D3D11CreateDevice")) return 1;

    Com<IDXGIDevice> dxgiDevice;
    Com<IDXGIAdapter> adapter;
    Com<IDXGIFactory2> factory;
    if (fail(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())), "QueryInterface(IDXGIDevice)")) return 1;
    if (fail(dxgiDevice->GetAdapter(adapter.put()), "GetAdapter")) return 1;
    if (fail(adapter->GetParent(IID_PPV_ARGS(factory.put())), "GetParent(IDXGIFactory2)")) return 1;

    DXGI_ADAPTER_DESC adapterDesc{};
    adapter->GetDesc(&adapterDesc);
    char gpuName[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, gpuName, sizeof(gpuName) - 1, nullptr, nullptr);

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = width;
    scd.Height = height;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Com<IDXGISwapChain1> swapChain;
    if (fail(factory->CreateSwapChainForHwnd(device.get(), g_hwnd, &scd, nullptr, nullptr, swapChain.put()),
             "CreateSwapChainForHwnd"))
        return 1;
    factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);

    Com<ID3D11RenderTargetView> rtv;
    Com<ID3D11DepthStencilView> dsv;
    auto createTargets = [&](UINT w, UINT h) -> bool {
        rtv.reset();
        dsv.reset();
        Com<ID3D11Texture2D> back;
        if (fail(swapChain->GetBuffer(0, IID_PPV_ARGS(back.put())), "GetBuffer")) return false;
        if (fail(device->CreateRenderTargetView(back.get(), nullptr, rtv.put()), "CreateRenderTargetView")) return false;

        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = w;
        dd.Height = h;
        dd.MipLevels = 1;
        dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.SampleDesc.Count = 1;
        dd.Usage = D3D11_USAGE_DEFAULT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        Com<ID3D11Texture2D> depth;
        if (fail(device->CreateTexture2D(&dd, nullptr, depth.put()), "CreateTexture2D(depth)")) return false;
        if (fail(device->CreateDepthStencilView(depth.get(), nullptr, dsv.put()), "CreateDepthStencilView")) return false;
        return true;
    };
    if (!createTargets(width, height)) return 1;

    // ---- Pipeline state ------------------------------------------------------
    auto compile = [&](const char* entry, const char* target, Com<ID3DBlob>& out) -> bool {
        Com<ID3DBlob> errors;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        const HRESULT r = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "vxe_viewer.hlsl", nullptr, nullptr,
                                     entry, target, flags, 0, out.put(), errors.put());
        if (errors && errors->GetBufferSize())
            std::fprintf(stderr, "[shader %s] %s\n", entry, (const char*)errors->GetBufferPointer());
        return !fail(r, "D3DCompile");
    };

    Com<ID3DBlob> vsBlob, psBlob, skyVsBlob, skyPsBlob, farPsBlob;
    if (!compile("VSMain", "vs_5_0", vsBlob) || !compile("PSMain", "ps_5_0", psBlob) ||
        !compile("VSSky", "vs_5_0", skyVsBlob) || !compile("PSSky", "ps_5_0", skyPsBlob))
        return 1;
    if (farTrace && !compile("PSFar", "ps_5_0", farPsBlob)) return 1;

    Com<ID3DBlob> ovVsBlob, ovPsBlob;
    Com<ID3D11VertexShader> ovVs;
    Com<ID3D11PixelShader> ovPs;
    if (!compile("VSOverlay", "vs_5_0", ovVsBlob) || !compile("PSOverlay", "ps_5_0", ovPsBlob)) return 1;
    device->CreateVertexShader(ovVsBlob->GetBufferPointer(), ovVsBlob->GetBufferSize(), nullptr, ovVs.put());
    device->CreatePixelShader(ovPsBlob->GetBufferPointer(), ovPsBlob->GetBufferSize(), nullptr, ovPs.put());

    Com<ID3D11BlendState> blendAlpha;
    {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&bd, blendAlpha.put());
    }
    Com<ID3D11SamplerState> overlaySampler;
    {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&sd, overlaySampler.put());
    }

    Com<ID3DBlob> tessVsBlob, hsBlob, dsBlob;
    Com<ID3D11VertexShader> tessVs;
    Com<ID3D11HullShader> hs;
    Com<ID3D11DomainShader> ds;
    if (tess) {
        if (!compile("VSTess", "vs_5_0", tessVsBlob) || !compile("HSMain", "hs_5_0", hsBlob) ||
            !compile("DSMain", "ds_5_0", dsBlob))
            return 1;
        device->CreateVertexShader(tessVsBlob->GetBufferPointer(), tessVsBlob->GetBufferSize(), nullptr, tessVs.put());
        device->CreateHullShader(hsBlob->GetBufferPointer(), hsBlob->GetBufferSize(), nullptr, hs.put());
        device->CreateDomainShader(dsBlob->GetBufferPointer(), dsBlob->GetBufferSize(), nullptr, ds.put());
    }

    Com<ID3D11VertexShader> vs, skyVs;
    Com<ID3D11PixelShader> ps, skyPs, farPs;
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, vs.put());
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, ps.put());
    device->CreateVertexShader(skyVsBlob->GetBufferPointer(), skyVsBlob->GetBufferSize(), nullptr, skyVs.put());
    device->CreatePixelShader(skyPsBlob->GetBufferPointer(), skyPsBlob->GetBufferSize(), nullptr, skyPs.put());
    if (farPsBlob) device->CreatePixelShader(farPsBlob->GetBufferPointer(), farPsBlob->GetBufferSize(), nullptr,
                                             farPs.put());

    // Matches vxe::TerrainVertex exactly, so mesher output uploads without a copy.
    static_assert(sizeof(TerrainVertex) == 28, "TerrainVertex layout changed; update the input elements");
    static_assert(sizeof(Vec3f) == 12, "Vec3f must be three tightly packed floats");
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"MATERIAL", 0, DXGI_FORMAT_R32_UINT,        0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    Com<ID3D11InputLayout> inputLayout;
    if (fail(device->CreateInputLayout(layout, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), inputLayout.put()),
             "CreateInputLayout"))
        return 1;

    auto makeCB = [&](size_t bytes, Com<ID3D11Buffer>& out) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = UINT((bytes + 15) & ~size_t(15));
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return !fail(device->CreateBuffer(&bd, nullptr, out.put()), "CreateBuffer(constant)");
    };
    Com<ID3D11Buffer> frameCB, chunkCB, farCB, overlayCB;
    if (!makeCB(sizeof(FrameCB), frameCB) || !makeCB(sizeof(ChunkCB), chunkCB) ||
        !makeCB(sizeof(OverlayCB), overlayCB))
        return 1;
    if (farTrace && !makeCB(sizeof(FarCB), farCB)) return 1;

    // DualContouring winds each quad so the right-hand-rule normal points out of
    // the solid. With this LH projection that is clockwise on screen for a face
    // turned toward the camera, i.e. D3D's default front face.
    D3D11_RASTERIZER_DESC rd{};
    rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    Com<ID3D11RasterizerState> rsSolid, rsNoCull, rsWire;
    rd.FillMode = D3D11_FILL_SOLID;
    device->CreateRasterizerState(&rd, rsSolid.put());
    rd.CullMode = D3D11_CULL_NONE;
    device->CreateRasterizerState(&rd, rsNoCull.put());
    rd.FillMode = D3D11_FILL_WIREFRAME;
    device->CreateRasterizerState(&rd, rsWire.put());

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_GREATER;   // reverse-Z
    Com<ID3D11DepthStencilState> dssTerrain;
    device->CreateDepthStencilState(&dsd, dssTerrain.put());
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    Com<ID3D11DepthStencilState> dssSky;
    device->CreateDepthStencilState(&dsd, dssSky.put());
    // The far field is drawn as a fullscreen pass after the meshed near field,
    // at depth 0. Under reverse-Z that is the far plane, so GREATER_EQUAL keeps
    // only the pixels no triangle reached -- the ray is never marched behind
    // terrain that is already on screen.
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    Com<ID3D11DepthStencilState> dssFar;
    device->CreateDepthStencilState(&dsd, dssFar.put());

    // ---- Engine --------------------------------------------------------------
    TaskSystem tasks;
    TerrainParams terrainParams;
    terrainParams.seed = seed;
    if (stripes) {
        terrainParams.stripeErosion = true;
        terrainParams.stripe.seed = seed;
        terrainParams.stripe.amplitude = stripeAmp;
        terrainParams.stripe.stripeWidth = stripeWidth;
        terrainParams.stripe.cellSize = stripeWidth * 4.f;
        // The stripes are meant to do the mid-scale work, not join in. Left at
        // full strength the hills FBm puts its own bumps across the gullies and
        // the result reads as lumps rather than drainage.
        terrainParams.hillHeight *= 0.25f;
        std::printf("stripes    : %d octaves, %.0f m amplitude, %.0f m between gullies, up to %.0f m of relief\n",
                    terrainParams.stripe.octaves, double(stripeAmp), double(stripeWidth),
                    double(stripeMaxOffset(terrainParams.stripe)));
    }
    DensityGenerator generator(terrainParams);
    DeltaStore deltas;

    StreamerConfig sc;
    sc.maxLod = uint8_t(maxLod);
    sc.viewDistance = viewDistance;
    sc.meshOpt.buildMeshlets = false;    // classic path: no task/mesh-shader clusters
    sc.meshOpt.buildCollision = false;   // no physics in this demo
    if (skirtCells >= 0.f) {             // --no-skirts / --skirt: see the raw inter-LOD cracks
        sc.mesherLod0.skirtDepthCells = skirtCells;
        sc.mesherFar.skirtDepthCells = skirtCells;
    }
    if (farTrace) {
        traceLod = std::clamp(traceLod, 1, maxLod);
        sc.traceLod = uint8_t(traceLod);
    }
    ChunkStreamer streamer(tasks, generator, deltas, sc);

    ChunkGpuRegistry gpuChunks(device.get());
    streamer.addListener(&gpuChunks);

    FarFieldBricks farBricks(device.get(), uint8_t(traceLod));
    if (farTrace) {
        if (!farBricks.init()) { std::fprintf(stderr, "far-field brick store failed to initialise\n"); return 1; }
        streamer.addListener(&farBricks);
        // Where the meshed shell ends: chunks finer than traceLod are split
        // while within splitFactor * their size, so this is the radius inside
        // which triangles exist -- and therefore where the ray may start.
        std::printf("far field  : LOD >= %d traced, meshed shell ends at %.0f m, atlas %zu MB for %u bricks\n",
                    traceLod, sc.splitFactor * double(world::kChunkCells << traceLod) * world::kVoxelSize,
                    farBricks.atlasBytes() / (1024 * 1024), FarFieldBricks::kSlotCount);
    }

    Terraformer terraformer(streamer, generator, deltas);
    VoxelTerrainQuery terrainQuery(streamer, deltas, generator);

    MaterialSystem materials;
    materials.registerDefaultTerrainMaterials();

    std::printf("gpu        : %s (feature level %x)\n", gpuName, unsigned(got));
    std::printf("workers    : %u\n", tasks.workerCount());
    std::printf("noise      : %s\n", generator.backendName());
    std::printf("chunks     : LOD0 %.0f m ... LOD%d %.0f m, view %.0f m\n", world::kChunkCells * world::kVoxelSize,
                sc.maxLod, double(world::kChunkCells << sc.maxLod), sc.viewDistance);

    // ---- Spawn + flyby path --------------------------------------------------
    auto findSpawn = [&]() {
        Vec3d p{2400.0, 0.0, -1800.0};
        for (int i = 0; i < 64; ++i) {
            const float h = generator.sampleHeight(float(p.x), float(p.z));
            if (h > 6.f && h < 220.f) { p.y = double(h); break; }
            p.x += 173.0;
            p.z += 97.0;
        }
        return p;
    };
    const Vec3d spawn = findSpawn();

    // A closed loop around the spawn. The radius and the clearance above ground
    // both wobble, so the camera alternately skims ridges and pulls back for a
    // wide shot; the look-at keys lead the position keys by ~1.5 waypoints.
    SplinePath flyPath, lookPath;
    {
        constexpr int kWaypoints = 10;
        std::vector<Vec3d> pos, look;
        pos.reserve(kWaypoints);
        look.reserve(kWaypoints);
        for (int i = 0; i < kWaypoints; ++i) {
            const double t = double(i) / double(kWaypoints);
            const double angle = t * 6.283185307;
            const double radius = 620.0 + 320.0 * std::sin(angle * 2.0 + 0.7);
            const Vec3d ground{spawn.x + std::cos(angle) * radius, 0.0, spawn.z + std::sin(angle) * radius};
            const double h = double(generator.sampleHeight(float(ground.x), float(ground.z)));
            const double clearance = 55.0 + 95.0 * (0.5 + 0.5 * std::sin(angle * 3.0 + 1.9));
            pos.push_back({ground.x, h + clearance, ground.z});
        }
        for (int i = 0; i < kWaypoints; ++i) {
            // Aim at the ground ~1.5 waypoints ahead, pulled down a little so the
            // horizon sits high and the terrain fills the frame.
            const Vec3d a = pos[size_t((i + 1) % kWaypoints)];
            const Vec3d b = pos[size_t((i + 2) % kWaypoints)];
            const Vec3d mid = (a + b) * 0.5;
            const double h = double(generator.sampleHeight(float(mid.x), float(mid.z)));
            look.push_back({mid.x, h + 25.0, mid.z});
        }
        flyPath.build(std::move(pos));
        lookPath.build(std::move(look));
        std::printf("flyby      : %.0f m loop around (%.0f, %.0f)\n", flyPath.totalLength(), spawn.x, spawn.z);
    }

    Camera camera;
    camera.position = {spawn.x, spawn.y + 120.0, spawn.z};
    camera.lookAt({spawn.x + 200.0, spawn.y, spawn.z + 200.0});
    if (fixedCam) {
        camera.position = {camArgs[0], camArgs[1], camArgs[2]};
        camera.yaw = float(camArgs[3] * 0.01745329252);
        camera.pitch = float(camArgs[4] * 0.01745329252);
    }
    std::printf("skirts     : %.1f cells (%.1f m at LOD0, %.1f m at LOD%d)\n", double(sc.mesherFar.skirtDepthCells),
                double(sc.mesherLod0.skirtDepthCells) * world::kVoxelSize,
                double(sc.mesherFar.skirtDepthCells) * double(1 << sc.maxLod) * world::kVoxelSize, sc.maxLod);

    bool flyby = startFlyby;
    bool wireframe = startWireframe;
    bool lodTint = startLodTint;
    bool cullBackfaces = true;
    bool tessOn = tess;            // T toggles it, when --tess built the shaders
    bool walkSnap = walk;          // first frame in walk mode places the feet rather than easing
    float digRadius = digRadiusStart;
    TextOverlay overlay(device.get());
    bool showOverlay = true;
    bool overlayDirty = true;
    double flyDistance = 0.0;
    double flySpeed = 55.0;        // metres/second along the loop
    float freeSpeed = 40.f;        // metres/second
    float fovY = 1.0472f;          // 60 degrees
    const float zNear = 0.1f;

    // Near-field submission list, rebuilt every frame; declared out here so the
    // allocation happens once rather than per frame.
    struct VisibleChunk { float dist2; const ChunkGpuRegistry::Entry* e; Vec3f rel; };
    std::vector<VisibleChunk> visible;
    visible.reserve(1024);

    Com<ID3D11Query> statsQuery;
    uint64_t statPixels = 0, statPrims = 0, statFrames = 0;
    if (pipeStats) {
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_PIPELINE_STATISTICS;
        if (FAILED(device->CreateQuery(&qd, statsQuery.put()))) return 1;
    }

    printControls();
    std::printf("streaming the first ring of chunks...\n");

    // ---- Main loop -----------------------------------------------------------
    auto prev = Clock::now();
    double titleTimer = 0.0, runTime = 0.0;
    double frameMsAvg = 16.0;
    uint64_t frameIndex = 0;
    bool shotTaken = false;
    std::string title;

    while (!g_input.quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_input.quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_input.quit) break;

        const auto now = Clock::now();
        float dt = float(std::chrono::duration<double>(now - prev).count());
        prev = now;
        dt = std::min(dt, 0.1f);   // a long hitch must not teleport the camera
        runTime += dt;

        if (g_input.resized) {
            g_input.resized = false;
            rtv.reset();
            dsv.reset();
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            swapChain->ResizeBuffers(0, g_input.width, g_input.height, DXGI_FORMAT_UNKNOWN, 0);
            if (!createTargets(g_input.width, g_input.height)) break;
            if (g_input.captured) { setCapture(false); setCapture(true); }
        }

        // ---- Input ------------------------------------------------------------
        if (g_input.takePress(VK_ESCAPE)) {
            if (g_input.captured) setCapture(false);
            else break;
        }
        if (g_input.takePress(VK_TAB)) setCapture(!g_input.captured);
        if (g_input.takePress('F')) {
            flyby = !flyby;
            if (flyby) {
                flyDistance = flyPath.nearestDistance(camera.position);
                std::printf("camera     : flyby, rejoining at %.0f m along the loop\n", flyDistance);
            } else {
                std::printf("camera     : free-cam\n");
            }
        }
        if (g_input.takePress('G')) wireframe = !wireframe;
        if (g_input.takePress('L')) lodTint = !lodTint;
        if (g_input.takePress('C')) cullBackfaces = !cullBackfaces;
        if (g_input.takePress('H')) showOverlay = !showOverlay;
        if (g_input.takePress('V')) {
            walk = !walk;
            if (walk) { flyby = false; walkSnap = true; }
            std::printf("camera     : %s\n", walk ? "walking, eyes at 2 m" : "free-cam");
        }
        if (g_input.takePress('T') && tess) {
            tessOn = !tessOn;
            std::printf("tessellation: %s\n", tessOn ? "on" : "off");
        }
        if (g_input.takePress('R')) {
            camera.position = {spawn.x, spawn.y + 120.0, spawn.z};
            camera.lookAt({spawn.x + 200.0, spawn.y, spawn.z + 200.0});
            flyby = false;
        }
        if (g_input.takePress(VK_OEM_4)) flySpeed = std::max(5.0, flySpeed - 10.0);    // '['
        if (g_input.takePress(VK_OEM_6)) flySpeed = std::min(400.0, flySpeed + 10.0);  // ']'
        if (g_input.takePress(VK_OEM_MINUS)) {                                         // '-'
            digRadius = std::max(1.f, digRadius - (digRadius > 6.f ? 2.f : 1.f));
            overlayDirty = true;
        }
        if (g_input.takePress(VK_OEM_PLUS)) {                                          // '='
            digRadius = std::min(24.f, digRadius + (digRadius >= 6.f ? 2.f : 1.f));
            overlayDirty = true;
        }
        if (g_input.wheel) {
            freeSpeed = std::clamp(freeSpeed * std::pow(1.25f, float(g_input.wheel)), 1.f, 4000.f);
            g_input.wheel = 0;
        }

        const bool wantsManualMove = g_input.keys['W'] || g_input.keys['A'] || g_input.keys['S'] ||
                                     g_input.keys['D'] || g_input.keys['Q'] || g_input.keys['E'];
        if (flyby && (wantsManualMove || (g_input.captured && (g_input.mouseDx || g_input.mouseDy)))) {
            flyby = false;   // any manual input drops out of the cinematic
            std::printf("camera     : free-cam (manual input)\n");
        }

        // ---- Camera -----------------------------------------------------------
        if (flyby && flyPath.valid()) {
            flyDistance += flySpeed * dt;
            camera.position = flyPath.atDistance(flyDistance);
            // Look slightly ahead of where the look spline is now, then ease the
            // angles rather than snapping, so the turns stay smooth.
            const Vec3d target = lookPath.atDistance(flyDistance);
            Camera want = camera;
            want.lookAt(target);
            const float k = 1.f - std::exp(-4.f * dt);
            camera.yaw = lerpAngle(camera.yaw, want.yaw, k);
            camera.pitch = lerp(camera.pitch, want.pitch, k);
        } else {
            if (g_input.captured) {
                constexpr float kSensitivity = 0.0022f;
                camera.yaw += float(g_input.mouseDx) * kSensitivity;
                camera.pitch -= float(g_input.mouseDy) * kSensitivity;
                camera.pitch = std::clamp(camera.pitch, -1.553f, 1.553f);   // just shy of straight up/down
            }
            // Walking: move in the horizontal plane whatever the view is doing,
            // and let the ground decide the height. Looking down must not walk
            // you into the dirt, which is the whole difference from free-cam.
            float speed = walk ? walkSpeed : freeSpeed;
            if (g_input.keys[VK_SHIFT]) speed *= walk ? 2.4f : 8.f;
            if (g_input.keys[VK_CONTROL]) speed *= walk ? 0.4f : 0.2f;
            Vec3f f = camera.forward(), r = camera.right();
            if (walk) {
                f.y = 0.f; r.y = 0.f;
                if (dot(f, f) > 1e-6f) f = normalize(f);
                if (dot(r, r) > 1e-6f) r = normalize(r);
            }
            Vec3f move{};
            if (g_input.keys['W']) move += f;
            if (g_input.keys['S']) move -= f;
            if (g_input.keys['D']) move += r;
            if (g_input.keys['A']) move -= r;
            if (!walk) {
                if (g_input.keys['E']) move.y += 1.f;
                if (g_input.keys['Q']) move.y -= 1.f;
            }
            if (dot(move, move) > 1e-6f) camera.position += Vec3d(normalize(move) * (speed * dt));

            if (walk) {
                // Entering walk mode from anywhere -- a flyby at altitude, say
                // -- the feet have to be put on the ground before a short probe
                // can find it, so the column height does the placing once.
                if (walkSnap) {
                    camera.position.y = double(generator.sampleHeight(float(camera.position.x),
                                                                      float(camera.position.z))) + double(eyeHeight);
                    walkSnap = false;
                }
                // Then a raycast each frame, rather than the column height,
                // because that one follows overhangs and anything dug since.
                const Vec3d probe{camera.position.x, camera.position.y + 3.0, camera.position.z};
                if (const auto hit = terrainQuery.raycast(probe, Vec3f{0.f, -1.f, 0.f}, 120.f)) {
                    const double target = probe.y - double(*hit) + double(eyeHeight);
                    camera.position.y += (target - camera.position.y) * (1.0 - std::exp(-14.0 * double(dt)));
                }
            }
            camera.position.y = std::clamp(camera.position.y, double(world::kWorldMinY) + 2.0, double(world::kWorldMaxY) - 2.0);
            const double limit = double(world::kWorldHalfExtentXZ) - 64.0;
            camera.position.x = std::clamp(camera.position.x, -limit, limit);
            camera.position.z = std::clamp(camera.position.z, -limit, limit);
        }
        g_input.mouseDx = g_input.mouseDy = 0;

        // ---- Terraform under the crosshair ------------------------------------
        if (g_input.dig || g_input.fill) {
            const bool dig = g_input.dig;
            g_input.dig = g_input.fill = false;
            const Vec3f dir = camera.forward();
            if (const auto hit = terrainQuery.raycast(camera.position, dir, 250.f)) {
                TerraformOp op;
                op.op = dig ? BrushOp::Subtract : BrushOp::Add;
                op.shape = BrushShape::Sphere;
                op.center = camera.position + Vec3d(dir * (*hit));
                op.halfExtents = Vec3f(digRadius);
                op.material = kMatDirt;
                const auto res = terraformer.apply(op);
                std::printf("%s at (%.0f, %.0f, %.0f): %u voxels, %u resident samples patched\n",
                            dig ? "dug" : "filled", op.center.x, op.center.y, op.center.z, res.voxelsWritten,
                            res.residentSamplesPatched);
            } else {
                // A miss used to be silent, which is indistinguishable from not
                // having clicked. At eye level you tend to look level or a
                // little up, and the ray then sails over the ground entirely.
                std::printf("%s missed: no terrain within 250 m of the crosshair (aim down at the ground)\n",
                            dig ? "dig" : "fill");
            }
        }

        // ---- Stream + upload ---------------------------------------------------
        streamer.update(camera.position);
        // First frames take everything so the opening shot is not half-empty;
        // after that the budget keeps a burst of completed chunks from hitching.
        gpuChunks.flushUploads(frameIndex < 4 ? 4096u : 32u);
        if (farTrace) farBricks.flushUploads(ctx.get(), frameIndex < 4 ? 4096u : 64u);
        materials.update(dt);

        // ---- Render ------------------------------------------------------------
        const float aspect = float(g_input.width) / float(std::max<UINT>(1, g_input.height));
        const Vec3f fwd = camera.forward(), rightV = camera.right(), upV = camera.up();
        const Mat4 viewProj = viewFromBasis(rightV, upV, fwd) * perspectiveReverseZInfinite(fovY, aspect, zNear);
        const Frustum frustum = frustumFrom(viewProj, fwd, zNear, float(sc.viewDistance) + 1200.f);

        FrameCB fcb{};
        std::memcpy(fcb.viewProj, viewProj.m, sizeof(fcb.viewProj));
        const Vec3f sun = normalize(Vec3f{0.42f, 0.62f, -0.36f});
        fcb.sunDir[0] = sun.x; fcb.sunDir[1] = sun.y; fcb.sunDir[2] = sun.z;
        fcb.fogDensity = 0.00020f;
        fcb.sunColor[0] = 1.28f; fcb.sunColor[1] = 1.16f; fcb.sunColor[2] = 0.98f;
        fcb.lodTint = lodTint ? 0.75f : 0.f;
        fcb.skyTop[0] = 0.26f; fcb.skyTop[1] = 0.45f; fcb.skyTop[2] = 0.86f;
        fcb.skyHorizon[0] = 0.72f; fcb.skyHorizon[1] = 0.81f; fcb.skyHorizon[2] = 0.92f;
        fcb.tanHalfFovY = std::tan(fovY * 0.5f);
        fcb.tanHalfFovX = fcb.tanHalfFovY * aspect;
        fcb.camRight[0] = rightV.x; fcb.camRight[1] = rightV.y; fcb.camRight[2] = rightV.z;
        fcb.camUp[0] = upV.x;       fcb.camUp[1] = upV.y;       fcb.camUp[2] = upV.z;
        fcb.camFwd[0] = fwd.x;      fcb.camFwd[1] = fwd.y;      fcb.camFwd[2] = fwd.z;
        {   // material palette straight out of the engine's own table
            const auto& table = materials.gpuTable();
            for (size_t i = 0; i < 8; ++i) {
                const bool has = i < table.size();
                fcb.material[i][0] = has ? table[i].baseColor[0] : 0.5f;
                fcb.material[i][1] = has ? table[i].baseColor[1] : 0.5f;
                fcb.material[i][2] = has ? table[i].baseColor[2] : 0.5f;
                fcb.material[i][3] = has ? table[i].roughness : 0.9f;
            }
        }
        // Pixels per radian: how long a given edge comes out on screen, which is
        // the only sane basis for a tessellation factor.
        fcb.pixPerRadian = float(g_input.height) * 0.5f / fcb.tanHalfFovY;
        fcb.tessTargetPx = tessTargetPx;
        fcb.tessMax = tessMax;
        fcb.dispFar = dispFar;
        fcb.dispFade = dispFar * 0.45f;
        fcb.dispAmp = tessOn ? dispAmp : 0.f;
        {
            D3D11_MAPPED_SUBRESOURCE map{};
            ctx->Map(frameCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
            std::memcpy(map.pData, &fcb, sizeof(fcb));
            ctx->Unmap(frameCB.get(), 0);
        }

        const D3D11_VIEWPORT vp{0.f, 0.f, float(g_input.width), float(g_input.height), 0.f, 1.f};
        ID3D11RenderTargetView* rtvs[] = {rtv.get()};
        ctx->OMSetRenderTargets(1, rtvs, dsv.get());
        ctx->RSSetViewports(1, &vp);
        ctx->ClearDepthStencilView(dsv.get(), D3D11_CLEAR_DEPTH, 0.f, 0);   // reverse-Z: far == 0

        ID3D11Buffer* cbs[] = {frameCB.get(), chunkCB.get()};
        ctx->VSSetConstantBuffers(0, 2, cbs);
        ctx->PSSetConstantBuffers(0, 2, cbs);

        // Sky first, depth off; it also clears the colour target.
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->OMSetDepthStencilState(dssSky.get(), 0);
        ctx->RSSetState(rsNoCull.get());   // the fullscreen triangle is wound the "wrong" way
        ctx->VSSetShader(skyVs.get(), nullptr, 0);
        ctx->PSSetShader(skyPs.get(), nullptr, 0);
        ctx->Draw(3, 0);

        // Terrain. With tessellation the same index buffer is reinterpreted as
        // 3-control-point patches; nothing about the mesh data changes.
        ctx->IASetInputLayout(inputLayout.get());
        ctx->IASetPrimitiveTopology(tessOn ? D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                           : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->OMSetDepthStencilState(dssTerrain.get(), 0);
        ctx->RSSetState(wireframe ? rsWire.get() : (cullBackfaces ? rsSolid.get() : rsNoCull.get()));
        ctx->VSSetShader(tessOn ? tessVs.get() : vs.get(), nullptr, 0);
        ctx->HSSetShader(tessOn ? hs.get() : nullptr, nullptr, 0);
        ctx->DSSetShader(tessOn ? ds.get() : nullptr, nullptr, 0);
        ctx->PSSetShader(ps.get(), nullptr, 0);
        if (tessOn) {
            ID3D11Buffer* tcbs[] = {frameCB.get(), chunkCB.get()};
            ctx->HSSetConstantBuffers(0, 2, tcbs);
            ctx->DSSetConstantBuffers(0, 2, tcbs);
        }

        uint32_t drawn = 0, culled = 0;
        uint64_t trianglesDrawn = 0;
        const UINT stride = sizeof(TerrainVertex), offset = 0;

        // Gather first, so the submission order is a choice rather than
        // whatever order the hash map happens to hold.
        visible.clear();
        for (const auto& [key, e] : gpuChunks.chunks()) {
            if (!e.vb || !e.ib || e.indexCount == 0) continue;
            const Vec3f rel = Vec3f(e.origin - camera.position);
            const Aabbf camRel{e.localBounds.min + rel, e.localBounds.max + rel};
            if (!frustum.intersects(camRel)) { ++culled; continue; }
            const Vec3f centre = (camRel.min + camRel.max) * 0.5f;
            visible.push_back({dot(centre, centre), &e, rel});
        }
        // Early-Z can only reject a fragment once something nearer has been
        // drawn, so hash order throws away most of what it could do.
        if (sortChunks)
            std::sort(visible.begin(), visible.end(),
                      [](const VisibleChunk& a, const VisibleChunk& b) { return a.dist2 < b.dist2; });

        if (pipeStats) ctx->Begin(statsQuery.get());
        for (const VisibleChunk& vc : visible) {
            const ChunkGpuRegistry::Entry& e = *vc.e;
            const Vec3f rel = vc.rel;

            ChunkCB ccb{{rel.x, rel.y, rel.z},
                        float(e.lod),
                        {float(e.origin.x), float(e.origin.y), float(e.origin.z)},
                        0.f};
            D3D11_MAPPED_SUBRESOURCE map{};
            ctx->Map(chunkCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
            std::memcpy(map.pData, &ccb, sizeof(ccb));
            ctx->Unmap(chunkCB.get(), 0);

            ID3D11Buffer* vbs[] = {e.vb.get()};
            ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
            ctx->IASetIndexBuffer(e.ib.get(), DXGI_FORMAT_R32_UINT, 0);
            ctx->DrawIndexed(e.indexCount, 0, 0);
            ++drawn;
            trianglesDrawn += e.indexCount / 3;
        }
        if (pipeStats) {
            ctx->End(statsQuery.get());
            D3D11_QUERY_DATA_PIPELINE_STATISTICS qs{};
            // Blocking read: this stalls the pipeline, which is exactly why it
            // lives behind --stats and not in the normal frame.
            while (ctx->GetData(statsQuery.get(), &qs, sizeof(qs), 0) == S_FALSE) {}
            statPixels += qs.PSInvocations;
            statPrims += qs.CInvocations;
            ++statFrames;
        }

        // Far field, last: a fullscreen pass that only touches pixels the
        // meshed shell left as sky, so the ray is never marched behind
        // geometry that already won the depth test.
        if (farTrace && farPs.get()) {
            // The fullscreen pass is triangles, not patches.
            ctx->HSSetShader(nullptr, nullptr, 0);
            ctx->DSSetShader(nullptr, nullptr, 0);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const Vec3i dim = farBricks.indexDim();
            const Vec3d org = farBricks.indexOriginWorld();
            const float chunkM = float(farBricks.chunkSizeVoxels()) * world::kVoxelSize;
            FarCB fcb{};
            fcb.camWorld[0] = float(camera.position.x);
            fcb.camWorld[1] = float(camera.position.y);
            fcb.camWorld[2] = float(camera.position.z);
            fcb.chunkSizeM = chunkM;
            fcb.worldMin[0] = float(org.x); fcb.worldMin[1] = float(org.y); fcb.worldMin[2] = float(org.z);
            fcb.voxelSizeM = float(1 << traceLod) * world::kVoxelSize;
            fcb.indexDims[0] = float(dim.x); fcb.indexDims[1] = float(dim.y); fcb.indexDims[2] = float(dim.z);
            fcb.farStart = 0.f;              // nothing is bricked inside the meshed shell anyway
            fcb.atlasDims[0] = float(FarFieldBricks::kSlotsX * kBrickSize);
            fcb.atlasDims[1] = float(FarFieldBricks::kSlotsY * kBrickSize);
            fcb.atlasDims[2] = float(FarFieldBricks::kSlotsZ * kBrickSize);
            fcb.farEnd = float(viewDistance);
            fcb.slotGrid[0] = float(FarFieldBricks::kSlotsX);
            fcb.slotGrid[1] = float(FarFieldBricks::kSlotsY);
            fcb.slotGrid[2] = float(FarFieldBricks::kSlotsZ);

            D3D11_MAPPED_SUBRESOURCE map{};
            ctx->Map(farCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
            std::memcpy(map.pData, &fcb, sizeof(fcb));
            ctx->Unmap(farCB.get(), 0);
            ID3D11Buffer* farCbs[] = {farCB.get()};
            ctx->PSSetConstantBuffers(2, 1, farCbs);

            farBricks.bind(ctx.get());
            ctx->IASetInputLayout(nullptr);
            ctx->OMSetDepthStencilState(dssFar.get(), 0);
            ctx->RSSetState(rsNoCull.get());
            ctx->VSSetShader(skyVs.get(), nullptr, 0);
            ctx->PSSetShader(farPs.get(), nullptr, 0);
            ctx->Draw(3, 0);
            FarFieldBricks::unbind(ctx.get());
        }

        // Overlay last, over everything, alpha blended.
        if (showOverlay) {
            if (overlayDirty || !overlay.srv()) {
                char buf[1024];
                std::snprintf(buf, sizeof(buf),
                              "H  hide this\n"
                              "\n"
                              "LMB / RMB   dig / fill      radius %.0f m   (- / = to resize)\n"
                              "W A S D     move,  Q / E up / down (free-cam only)\n"
                              "Shift       faster,  Ctrl  slower,  wheel  free-cam speed\n"
                              "Tab         capture the mouse.  Esc releases, then quits\n"
                              "\n"
                              "V   walk / free-cam        F   cinematic flyby\n"
                              "G   wireframe              L   LOD colour tint\n"
                              "C   backface culling       R   respawn\n"
                              "T   tessellation           [ / ]  flyby speed",
                              double(digRadius));
                overlay.build(buf, 520, 17);
                overlayDirty = false;
            }
            if (overlay.srv()) {
                // Pin to the top-left in pixels, whatever the window size.
                const float w = 2.f * float(overlay.width()) / float(g_input.width);
                const float h = 2.f * float(overlay.height()) / float(g_input.height);
                const float m = 0.018f;
                OverlayCB ocb{{-1.f + m, 1.f - m * (float(g_input.width) / float(g_input.height)),
                               -1.f + m + w, 1.f - m * (float(g_input.width) / float(g_input.height)) - h}};
                D3D11_MAPPED_SUBRESOURCE map{};
                ctx->Map(overlayCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
                std::memcpy(map.pData, &ocb, sizeof(ocb));
                ctx->Unmap(overlayCB.get(), 0);

                ID3D11Buffer* ocbs[] = {overlayCB.get()};
                ctx->VSSetConstantBuffers(3, 1, ocbs);
                ctx->PSSetConstantBuffers(3, 1, ocbs);
                ID3D11ShaderResourceView* srvs[] = {overlay.srv()};
                ctx->PSSetShaderResources(2, 1, srvs);
                ID3D11SamplerState* smps[] = {overlaySampler.get()};
                ctx->PSSetSamplers(1, 1, smps);

                ctx->IASetInputLayout(nullptr);
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx->HSSetShader(nullptr, nullptr, 0);
                ctx->DSSetShader(nullptr, nullptr, 0);
                ctx->OMSetDepthStencilState(dssSky.get(), 0);
                ctx->OMSetBlendState(blendAlpha.get(), nullptr, 0xFFFFFFFF);
                ctx->RSSetState(rsNoCull.get());
                ctx->VSSetShader(ovVs.get(), nullptr, 0);
                ctx->PSSetShader(ovPs.get(), nullptr, 0);
                ctx->Draw(3, 0);
                ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                ID3D11ShaderResourceView* none[] = {nullptr};
                ctx->PSSetShaderResources(2, 1, none);
            }
        }

        // Capture before Present: with FLIP_DISCARD the back buffer's contents
        // are undefined afterwards.
        if (!shotPath.empty() && !shotTaken && runTime >= shotAt) {
            shotTaken = true;
            std::printf("captured after %.1f s: %u chunks resident, %u drawn, %.2f M triangles\n", runTime,
                        streamer.stats().residentChunks, drawn, double(trianglesDrawn) / 1e6);
            saveScreenshot(device.get(), ctx.get(), swapChain.get(), shotPath.c_str());
            if (exitAfter <= 0.0) break;
        }

        swapChain->Present(vsync ? 1 : 0, 0);
        ++frameIndex;
        if (exitAfter > 0.0 && runTime >= exitAfter) break;

        // ---- Title bar stats ----------------------------------------------------
        frameMsAvg = frameMsAvg * 0.92 + double(dt) * 1000.0 * 0.08;
        titleTimer += dt;
        if (titleTimer > 0.2) {
            titleTimer = 0.0;
            const auto& s = streamer.stats();
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                          "VXE viewer | %s | %.1f fps (%.1f ms) | pos %.0f %.0f %.0f | chunks %u res / %u drawn / %u culled"
                          " | %.2f M tris | building %u, queued %zu%s%s",
                          flyby  ? "FLYBY (F for free-cam)"
                          : walk ? "WALK 2 m (V for free-cam, Tab for mouse)"
                                 : "FREE-CAM (V to walk, F for flyby, Tab for mouse)",
                          frameMsAvg > 0.0 ? 1000.0 / frameMsAvg : 0.0, frameMsAvg, camera.position.x, camera.position.y,
                          camera.position.z, s.residentChunks, drawn, culled, double(trianglesDrawn) / 1e6,
                          s.buildingChunks, gpuChunks.pendingUploads(), wireframe ? " | wire" : "",
                          lodTint ? " | lod-tint" : "");
            title = buf;
            SetWindowTextA(g_hwnd, title.c_str());
        }
    }

    setCapture(false);
    std::printf("\nshutting down (%llu frames, %.1f s, %llu tasks executed)\n", (unsigned long long)frameIndex, runTime,
                (unsigned long long)tasks.tasksExecuted());
    std::printf("final camera: (%.1f, %.1f, %.1f) yaw %.1f deg pitch %.1f deg, mode %s\n", camera.position.x,
                camera.position.y, camera.position.z, camera.yaw * 57.2957795f, camera.pitch * 57.2957795f,
                flyby ? "flyby" : (walk ? "walk" : "free"));
    if (pipeStats && statFrames) {
        const double px = double(statPixels) / double(statFrames);
        const double screen = double(g_input.width) * double(g_input.height);
        std::printf("near field  : %.2f M pixel-shader invocations per frame over %.2f M screen pixels"
                    " = %.2fx overdraw, %.2f M triangles rasterised, submission %s\n",
                    px / 1e6, screen / 1e6, px / screen, double(statPrims) / double(statFrames) / 1e6,
                    sortChunks ? "front-to-back" : "hash order");
    }
    if (farTrace)
        std::printf("far field   : %zu bricks resident (%zu MB of a %zu MB atlas), %llu uploads over %llu frames%s\n",
                    farBricks.resident(), farBricks.usedBytes() / (1024 * 1024),
                    farBricks.atlasBytes() / (1024 * 1024), (unsigned long long)farBricks.totalUploads(),
                    (unsigned long long)frameIndex, farBricks.overflowed() ? "  [ATLAS OVERFLOWED]" : "");
    std::printf("frame time  : %.3f ms mean over %llu frames (%.1f fps)%s\n", runTime * 1000.0 / double(frameIndex),
                (unsigned long long)frameIndex, double(frameIndex) / runTime, vsync ? " [vsync on]" : "");
    CoUninitialize();
    return 0;
}
