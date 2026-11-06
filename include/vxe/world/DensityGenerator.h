#pragma once
// -----------------------------------------------------------------------------
// DensityGenerator: procedural base density for any chunk at any LOD.
//
// Pipeline per chunk (all SIMD batch calls into FastNoise2):
//
//   2D (38x38 columns, once per chunk)        3D (38^3, only where needed)
//   ---------------------------------        --------------------------------
//   continental FBm  --spline-->  base h      overhang FBm  (surface band only)
//   mountain mask FBm --mask-->  ridged h     spaghetti A,B (caves, LOD <= 1)
//   hills FBm        ---------->  detail h    cheese FBm    (caverns, LOD <= 1)
//   cave entrance FBm
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

#include "vxe/world/DensityField.h"

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
    float spaghettiScale = 110.f;
    float spaghettiRadius = 0.08f;     // in noise units, ~4 m tunnels
    float cheeseScale = 220.f;
    float cheeseThreshold = 0.55f;
    float caveSurfaceShell = 10.f;     // metres of rock kept above caves outside entrances
    float bedrockY = -480.f;
    int32_t maxCaveLod = 1;            // caves are not generated for coarser LODs
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

    const TerrainParams& params() const { return m_params; }
    // Max vertical displacement 3D noise can add above/below the 2D height.
    float verticalSlack() const;
    const char* backendName() const;

private:
    struct Impl;
    TerrainParams m_params;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vxe
