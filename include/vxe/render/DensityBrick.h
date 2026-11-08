#pragma once
// -----------------------------------------------------------------------------
// DensityBrick: a chunk's density field, quantised for the far-field trace path.
//
// The hybrid renderer meshes the near field, where collision and crisp detail
// are wanted, and traces the far field, where neither is. The halves come from
// the same ChunkStreamer, and so from the same generator and the same
// DeltaStore -- which is the property that makes them agree at the handover. A
// brick is the chunk's own samples requantised, not a second opinion about the
// terrain, so the traced surface meets the meshed one wherever the two touch.
//
// Layout: 34^3 samples covering local points [-1, 32] -- the chunk's 33 corners
// plus a one-sample border on each side, so a trilinear fetch anywhere inside
// the chunk finds its neighbours without reaching into an adjacent brick (which
// may be at a different LOD, or absent).
//
// unorm8 over [-kDensityClamp, +kDensityClamp]: 8 cm of precision on the field,
// against a far-field voxel of 8 m or more. The surface position is carried by
// where the samples cross 128, not by their precision.
// -----------------------------------------------------------------------------

#include "vxe/world/DensityField.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace vxe {

inline constexpr int32_t kBrickSize   = 34;
inline constexpr int32_t kBrickVolume = kBrickSize * kBrickSize * kBrickSize;

struct DensityBrick {
    // Interleaved as the GPU wants it: two channels per sample, density then
    // material, so one R8G8 upload carries both. Density is filtered on the way
    // out; material is a palette index and must not be, so the shader point
    // loads it.
    std::array<uint8_t, size_t(kBrickVolume) * 2> texels{};
    // Entirely solid or entirely air: nothing for the trace to find, so the
    // slot is never allocated and the ray skips the whole chunk.
    bool uniform = true;

    static constexpr int32_t index(int32_t x, int32_t y, int32_t z) {
        return x + kBrickSize * (y + kBrickSize * z);
    }
};

// Requantises the sub-cube of `field` the trace needs. Called off the main
// thread, in the slot the mesher would otherwise have occupied.
inline void fillBrick(const DensityField& field, DensityBrick& out) {
    constexpr float kScale = 127.5f / kDensityClamp;
    bool anySolid = false, anyAir = false;
    for (int32_t z = 0; z < kBrickSize; ++z)
        for (int32_t y = 0; y < kBrickSize; ++y)
            for (int32_t x = 0; x < kBrickSize; ++x) {
                // Brick sample i is the chunk's local point i - 1.
                const size_t src = size_t(DensityField::pointIndex(x - 1, y - 1, z - 1));
                const float d = field.density[src];
                anySolid |= d < 0.f;
                anyAir   |= d >= 0.f;
                const size_t dst = size_t(DensityBrick::index(x, y, z)) * 2;
                out.texels[dst]     = uint8_t(std::clamp(127.5f + d * kScale, 0.f, 255.f));
                out.texels[dst + 1] = uint8_t(field.material[src]);
            }
    out.uniform = !(anySolid && anyAir);
}

} // namespace vxe
