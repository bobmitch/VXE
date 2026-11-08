#pragma once
// -----------------------------------------------------------------------------
// DensityGenerator: procedural base density for any chunk at any LOD.
//
// Pipeline per chunk (all SIMD batch calls into FastNoise2):
//
//   2D (38x38 columns, once per chunk)        3D (38^3, only where needed)
//   ---------------------------------        --------------------------------
//   continental FBm  --spline-->  base h      overhang FBm  (surface band only)
//   mountain mask FBm --mask-->  ridged h     placed caves  (CaveNetwork, SDF
//   hills FBm        ---------->  detail h                   splat, not noise)
//   stripe erosion   --gullies->  eroded h
//
//   density = clamp( max( (y - h) - overhang, -caveSdf ), +-kDensityClamp )
//
// Column-first evaluation is the big win: the expensive multi-octave terrain
// shaping runs on 38^2 points, not 38^3. 3D noise is skipped entirely for
// chunks that are provably all-air or all-rock-without-caves.
//
// Determinism: positions fed to FastNoise2 are integer metres (offset + i*step,
// step = 2^lod), exactly representable in float, and FastNoise2 is built with
// FASTNOISE2_STRICT_FP. Adjacent chunks therefore produce bit-identical
// samples on shared points, which is what makes chunk seams crack-free.
//
// Thread safety: generate()/sample() are const and re-entrant; scratch memory
// is per-thread.
// -----------------------------------------------------------------------------

#include "vxe/world/CaveNetwork.h"
#include "vxe/world/DensityField.h"
#include "vxe/world/StripeErosion.h"

#include <memory>

namespace vxe {

struct TerrainParams {
    int32_t seed = 1337;
    float seaLevel = 0.f;
    float continentalScale = 6000.f;   // metres per feature
    float mountainMaskScale = 3200.f;
    float ridgeScale = 900.f;
    float ridgeHeight = 700.f;
    float hillScale = 180.f;
    float hillHeight = 10.f;
    float overhangScale = 48.f;
    float overhangAmpPlains = 2.5f;
    float overhangAmpMountains = 14.f;
    float bedrockY = -480.f;

    // Where the caves themselves come from. Placed and routed by CaveNetwork,
    // not thresholded out of noise -- see its header, and §9 of the world
    // design doc for why the noise fields went.
    CaveParams cave{};

    // Caves. On again, but placed rather than thresholded: the noise fields
    // this replaces left about 18% of all subsurface rock hollow, uniformly,
    // everywhere, and could not have done otherwise (§9, failure 1).
    bool caves = true;

    // Caves past this LOD are not generated at all. At 1 that was a cliff
    // rather than a fade: every cave in a whole ring of chunks existed on one
    // side of the LOD1/LOD2 boundary and not on the other, and the ring swept
    // across the world as the camera moved.
    //
    // 3 rather than 4 is measured. Thresholded noise does not coarsen into
    // fewer, larger voids -- it fragments into blobs -- so carrying it outward
    // costs 4-6x the triangles of solid rock at every level, and at LOD4 it was
    // 12x for caves that are sub-pixel at the 1 km such a chunk implies. LOD4
    // stays solid; LOD2 and LOD3 carry the fade.
    int32_t maxCaveLod = 3;

    // A void only survives sampling at s metres while it is wide enough for a
    // sample to land inside it, so coarse levels drop thin caves whatever the
    // operator. caveSdf is a distance in metres, so they can be widened to suit
    // -- but only as far as they fall short, and only the term that is actually
    // thin.
    //
    // Keep a tunnel at least this many half-sample-spacings wide:
    //
    //   dilation = max(0, stride/2 * margin - tunnelRadius)
    //
    // The subtraction is what makes it adaptive. A first attempt widened every
    // void by a flat amount per level, which put the same metres on a 4 m
    // tunnel as on a 40 m cavern -- +75% against +7% -- so the size *ordering*
    // of what you saw at range stopped matching what you found up close. Here a
    // feature already wider than the sampling gets nothing, which zeroes this
    // at LOD0 through LOD2 and leaves only LOD3 widened, and the cheese term is
    // never touched at all: its features run to tens of metres and were never
    // the ones falling through.
    //
    // Still a trade. Tunnels at LOD3 read wider than they are and tighten as
    // you approach, and since dilating an SDF grows every negative region, rock
    // thinner than the dilation erodes there. Parametric voids are the real
    // answer -- a capsule that knows its own radius needs none of this.
    float caveDilationMargin = 1.5f;

    // At LOD >= 2 only caves within this depth of the surface are evaluated.
    // Deep caves cost three 3D noise fields over a 38^3 block to generate and
    // cannot be seen from the >= 256 m such a chunk implies, because there is
    // rock in the way.
    float coarseCaveDepth = 160.f;

    // Gradient-aligned stripe erosion over the column heights: gullies and
    // ridges that follow the slope, in place of the hills FBm doing all the
    // mid-scale work. Off by default, because switching it on moves every
    // height in the world and so re-bases every DeltaStore edit -- it is a
    // generator version change in the sense §8 of docs/world-design.md means.
    bool stripeErosion = false;
    StripeParams stripe{};
};

struct ChunkGenStats {
    bool uniform = false;          // all air or all solid: mesher can skip
    bool evaluated3D = false;      // whether any 3D noise ran
    float minHeight = 0.f, maxHeight = 0.f;
};

struct HeightRange { float minY, maxY; };

class DensityGenerator {
public:
    explicit DensityGenerator(const TerrainParams& params = {});
    ~DensityGenerator();
    DensityGenerator(const DensityGenerator&) = delete;
    DensityGenerator& operator=(const DensityGenerator&) = delete;

    // Fills a padded DensityField for the chunk. Thread-safe.
    ChunkGenStats generate(const ChunkKey& key, DensityField& out) const;

    // Single-point evaluation at a LOD0 voxel corner (slow path; edits/queries
    // on non-resident points).
    float sampleDensity(const Vec3i& voxel) const;

    // Surface height (no caves/overhangs) - used for spawn placement and AI.
    float sampleHeight(float x, float z) const;

    // Conservative height bounds over an XZ rectangle (for streaming culling).
    HeightRange columnRange(float x0, float z0, float x1, float z1) const;

    // Cave mouths within `radius` of a point, world space. For anything that
    // needs to know where a way in is -- a debug marker now, reconnaissance or
    // spawn placement later -- without walking the passages behind them.
    void caveEntrancesNear(const Vec3d& centre, float radius, std::vector<Vec3f>& out) const;

    const TerrainParams& params() const { return m_params; }
    // Max vertical displacement 3D noise can add above/below the 2D height.
    float verticalSlack() const;
    const char* backendName() const;

private:
    // Gradient of the unfiltered height at a fixed world-space epsilon, which
    // is what keeps the gully pattern identical at every LOD.
    SlopeXZ baseGradient(float x, float z) const;

    // The cave system for one region cell, generated on first use and cached
    // per thread. Empty for the cells that hold none, which is most of them.
    const CaveSystem& cachedSystem(int32_t cellX, int32_t cellZ) const;

    struct Impl;
    TerrainParams m_params;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vxe
