#pragma once
// -----------------------------------------------------------------------------
// Terrain queries used by gameplay systems that must not depend on terrain
// internals (building anchors, foot IK, AI placement).
//
//   VoxelTerrainQuery : proximity lookups straight from resident LOD0 density
//                       (trilinear), falling back to the generator + deltas.
//                       Main thread; exact w.r.t. the *simulation* field, and
//                       available the instant an edit lands (no re-bake lag).
//   PhysicsTerrainQuery (apps): Jolt raycasts against baked collision; thread-
//                       safe, includes structures and props, lags edits by one
//                       bake (~1 frame).
// -----------------------------------------------------------------------------

#include "vxe/core/Math.h"

#include <optional>

namespace vxe {

class ChunkStreamer;
class DeltaStore;
class DensityGenerator;

class ITerrainQuery {
public:
    virtual ~ITerrainQuery() = default;
    // Signed density (metres, < 0 solid) at an arbitrary world point.
    virtual float density(const Vec3d& p) const = 0;
    // Distance to the first solid surface straight down, if within maxDistance.
    virtual std::optional<float> raycastDown(const Vec3d& from, float maxDistance) const;
    // Generic ray march; returns hit distance.
    virtual std::optional<float> raycast(const Vec3d& from, const Vec3f& dir, float maxDistance) const;
};

class VoxelTerrainQuery final : public ITerrainQuery {
public:
    VoxelTerrainQuery(const ChunkStreamer& streamer, const DeltaStore& deltas, const DensityGenerator& gen)
        : m_streamer(streamer), m_deltas(deltas), m_gen(gen) {}
    float density(const Vec3d& p) const override;

private:
    float latticeDensity(const Vec3i& v) const;
    const ChunkStreamer& m_streamer;
    const DeltaStore& m_deltas;
    const DensityGenerator& m_gen;
};

} // namespace vxe
