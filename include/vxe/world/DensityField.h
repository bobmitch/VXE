#pragma once
// -----------------------------------------------------------------------------
// DensityField: the padded sample buffer a chunk is generated into and meshed
// from.
//
// Convention: density is a clamped, approximately-metric signed distance.
//     density <  0  -> solid
//     density >= 0  -> air
//
// Layout: N = 32 cells per edge. We sample S = N + 6 = 38 points per axis,
// starting two points *before* the chunk origin:
//
//     sample index s  ->  local grid point  p = s - 2    (p in [-2, N+3])
//
//   * points [0, N+1]  -> corners of the N+1 cells the mesher visits
//                         (cell N is shared with the +neighbour, giving seamless joins)
//   * points -2..-1, N+2..N+3 -> gradient stencil ring. The mesher's ENO
//                         gradients need p-2..p+2 for every corner point; with
//                         a full ring, the stencil (and therefore every shared
//                         seam vertex) is identical in both neighbouring chunks.
//
// For LOD L chunks, grid point p is LOD0 voxel (origin + p * 2^L).
// -----------------------------------------------------------------------------

#include "vxe/core/WorldCoords.h"

#include <array>
#include <cstdint>

namespace vxe {

inline constexpr int32_t kFieldCells  = world::kChunkCells;     // 32
inline constexpr int32_t kFieldSize   = kFieldCells + 6;        // 38 samples per axis
inline constexpr int32_t kFieldOffset = 2;                      // sample index of local point 0
inline constexpr int32_t kFieldVolume = kFieldSize * kFieldSize * kFieldSize;
inline constexpr float   kDensityClamp = 8.0f;                  // metres; also the int16 quantisation range

using MaterialId = uint8_t;

enum TerrainMaterial : MaterialId {
    kMatAir = 0,
    kMatGrass,
    kMatDirt,
    kMatRock,
    kMatSand,
    kMatSnow,
    kMatDeepstone,
    kMatGravel,
    kMatCount
};


struct alignas(64) DensityField {
    std::array<float, kFieldVolume> density;
    std::array<MaterialId, kFieldVolume> material;

    static constexpr int32_t index(int32_t sx, int32_t sy, int32_t sz) {
        return sx + kFieldSize * (sy + kFieldSize * sz);
    }
    // Index by *local grid point* (p = s - 1).
    static constexpr int32_t pointIndex(int32_t px, int32_t py, int32_t pz) {
        return index(px + kFieldOffset, py + kFieldOffset, pz + kFieldOffset);
    }
    float at(int32_t px, int32_t py, int32_t pz) const { return density[size_t(pointIndex(px, py, pz))]; }

    // True if every sample has the same sign: nothing to mesh.
    bool isUniform() const {
        const bool s0 = density[0] < 0.f;
        for (float d : density)
            if ((d < 0.f) != s0) return false;
        return true;
    }
};

static_assert(sizeof(DensityField) <= 280 * 1024, "DensityField budget exceeded");   // 268 KB

} // namespace vxe
