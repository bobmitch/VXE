#pragma once
// -----------------------------------------------------------------------------
// Terraformer: real-time CSG edits of the density field.
//
// apply() runs on the main thread and is O(brush volume):
//   1. read current density for every LOD0 lattice point under the brush
//      (resident field -> delta store -> generator slow path)
//   2. combine with the brush SDF (smooth CSG), quantise, write to DeltaStore
//      under a single exclusive lock
//   3. patch every resident LOD0 field whose *padded* range contains the
//      point, so neighbours' seam samples stay identical
//   4. invalidate overlapping chunks at all LODs (Critical priority remesh,
//      which flows on to GPU upload and Jolt re-bake automatically)
//   5. notify listeners (BuildingSystem re-checks foundations in the region)
// -----------------------------------------------------------------------------

#include "vxe/core/Math.h"
#include "vxe/world/DensityField.h"

#include <cstdint>
#include <vector>

namespace vxe {

class ChunkStreamer;
class DeltaStore;
class DensityGenerator;

enum class BrushShape : uint8_t { Sphere, Box, Cylinder };
enum class BrushOp : uint8_t {
    Subtract,   // dig
    Add,        // fill / build up terrain
    Flatten,    // pull toward the horizontal plane through center.y (Valheim hoe)
    Smooth,     // relax toward the local average
    Paint       // material only
};

struct TerraformOp {
    BrushShape shape = BrushShape::Sphere;
    BrushOp op = BrushOp::Subtract;
    Vec3d center{};
    Vec3f halfExtents{2.f, 2.f, 2.f};   // sphere: x = radius; cylinder: x = radius, y = half height
    float strength = 1.f;               // 0..1 blend toward the target field
    float smoothK = 0.6f;               // smooth-CSG radius in metres (0 = hard edges, keeps DC creases)
    MaterialId material = kMatDirt;
};

struct TerraformResult {
    Aabbd affected{};
    uint32_t voxelsWritten = 0;
    uint32_t residentSamplesPatched = 0;
};

class ITerrainEditListener {
public:
    virtual ~ITerrainEditListener() = default;
    virtual void onTerrainEdited(const Aabbd& region) = 0;
};

class Terraformer {
public:
    Terraformer(ChunkStreamer& streamer, const DensityGenerator& generator, DeltaStore& deltas);

    TerraformResult apply(const TerraformOp& op);
    void addListener(ITerrainEditListener* l) { m_listeners.push_back(l); }

    // Signed distance of the brush volume (negative inside). Public for tools/preview.
    static float brushSdf(const TerraformOp& op, const Vec3d& p);

private:
    float currentDensity(const Vec3i& voxel) const;

    ChunkStreamer& m_streamer;
    const DensityGenerator& m_generator;
    DeltaStore& m_deltas;
    std::vector<ITerrainEditListener*> m_listeners;
};

} // namespace vxe
