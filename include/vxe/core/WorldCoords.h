#pragma once
// -----------------------------------------------------------------------------
// World coordinate model for a ~512 km^2 world (22.6 km x 22.6 km).
//
// Precision budget: a 32-bit float has a 24-bit mantissa, so at |x| = 11.3 km
// the representable step is ~0.98 mm. That is fine for *storage* of chunk-local
// data but causes visible vertex jitter in clip space and contact jitter in
// physics when used as an absolute coordinate. We therefore split coordinates:
//
//   * Authoritative positions (entities, camera, physics bodies) : double (Vec3d)
//   * Terrain addressing                                         : int32 voxel / chunk coords
//   * Mesh vertices                                              : float, *chunk-local* (|v| <= 32 m)
//   * GPU                                                        : camera-relative float
//                                                                  (chunkOrigin - cameraPos computed in double,
//                                                                   then narrowed; no origin rebasing needed)
//   * Jolt                                                       : JPH_DOUBLE_PRECISION (RVec3 body positions,
//                                                                  float shapes relative to body origin)
//
// FloatingOrigin is still provided for subsystems that insist on float world
// space (audio engines, particle sims, middleware): they observe rebase events.
// -----------------------------------------------------------------------------

#include "vxe/core/Math.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace vxe {

namespace world {
// 707 chunks * 32 m = 22,624 m per side -> 511.8 km^2.
inline constexpr int32_t kChunkCells        = 32;            // cells per chunk edge at LOD0
inline constexpr float   kVoxelSize         = 1.0f;          // metres per LOD0 cell
inline constexpr int32_t kWorldHalfExtentXZ = 11312;         // metres, |x|,|z| <= this
inline constexpr int32_t kWorldMinY         = -512;          // metres
inline constexpr int32_t kWorldMaxY         = 1536;          // metres
inline constexpr int32_t kMaxLod            = 6;             // LOD6 chunk = 2048 m
} // namespace world

// Integer address of a terrain chunk. lod = 0 is full resolution; a LOD L chunk
// covers 2^L x 2^L x 2^L LOD0 chunks and samples density at a stride of 2^L voxels.
struct ChunkKey {
    int32_t x = 0, y = 0, z = 0;
    uint8_t lod = 0;

    constexpr bool operator==(const ChunkKey&) const = default;

    constexpr int32_t strideVoxels() const { return 1 << lod; }
    constexpr int32_t sizeVoxels() const { return world::kChunkCells << lod; }

    // LOD0 voxel coordinate of this chunk's minimum corner.
    constexpr Vec3i originVoxel() const { return {x * sizeVoxels(), y * sizeVoxels(), z * sizeVoxels()}; }
    Vec3d originWorld() const { return Vec3d(originVoxel()) * double(world::kVoxelSize); }
    Aabbd boundsWorld() const {
        const Vec3d o = originWorld();
        return {o, o + Vec3d(double(sizeVoxels()) * world::kVoxelSize)};
    }
    constexpr ChunkKey parent() const {
        return {floorDiv(x, 2), floorDiv(y, 2), floorDiv(z, 2), uint8_t(lod + 1)};
    }
    constexpr ChunkKey child(int i) const {
        return {x * 2 + (i & 1), y * 2 + ((i >> 1) & 1), z * 2 + ((i >> 2) & 1), uint8_t(lod - 1)};
    }
    static ChunkKey containing(const Vec3d& worldPos, uint8_t lod) {
        const double size = double(world::kChunkCells << lod) * world::kVoxelSize;
        return {int32_t(std::floor(worldPos.x / size)), int32_t(std::floor(worldPos.y / size)),
                int32_t(std::floor(worldPos.z / size)), lod};
    }
};

struct ChunkKeyHash {
    size_t operator()(const ChunkKey& k) const noexcept {
        uint64_t h = uint64_t(uint32_t(k.x)) | (uint64_t(uint32_t(k.z)) << 32);
        h = mix64(h ^ (uint64_t(uint32_t(k.y)) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(k.lod) << 59));
        return size_t(h);
    }
};

// Camera-relative transform: computed in double, narrowed once per draw.
inline Vec3f toCameraRelative(const Vec3d& worldPos, const Vec3d& cameraPos) {
    return Vec3f(worldPos - cameraPos);
}

// Optional float-space origin for subsystems that cannot run in double.
class FloatingOrigin {
public:
    using RebaseListener = std::function<void(const Vec3d& oldOrigin, const Vec3d& newOrigin)>;

    explicit FloatingOrigin(double rebaseDistance = 2048.0) : m_rebaseDistance(rebaseDistance) {}

    // Snap to a coarse grid so rebases are rare and deterministic.
    bool update(const Vec3d& focus) {
        const Vec3d d = focus - m_origin;
        if (std::abs(d.x) < m_rebaseDistance && std::abs(d.y) < m_rebaseDistance && std::abs(d.z) < m_rebaseDistance)
            return false;
        const double g = m_rebaseDistance * 0.5;
        const Vec3d newOrigin{std::round(focus.x / g) * g, std::round(focus.y / g) * g, std::round(focus.z / g) * g};
        const Vec3d old = m_origin;
        m_origin = newOrigin;
        for (auto& l : m_listeners) l(old, newOrigin);
        return true;
    }
    void addListener(RebaseListener l) { m_listeners.push_back(std::move(l)); }
    const Vec3d& origin() const { return m_origin; }
    Vec3f toLocal(const Vec3d& p) const { return Vec3f(p - m_origin); }
    Vec3d toWorld(const Vec3f& p) const { return m_origin + Vec3d(p); }

private:
    double m_rebaseDistance;
    Vec3d m_origin{};
    std::vector<RebaseListener> m_listeners;
};

} // namespace vxe
