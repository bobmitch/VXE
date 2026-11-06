#pragma once
// -----------------------------------------------------------------------------
// DeltaStore: persistent sparse overrides of the procedural density field.
//
// The world is never saved as voxels. The procedural generator is the source
// of truth; only player modifications are stored, as per-LOD0-chunk override
// sets keyed by the chunk that *owns* the voxel (32^3 owned points per chunk,
// so a local index fits in 16 bits).
//
// In-memory representation (per chunk):
//   * Sparse : sorted vector<{uint16 idx, int16 density, uint8 material}>, 5 B/entry
//   * Dense  : bitmask (4 KB) + full int16/uint8 arrays (96 KB), switched to
//              automatically above kDenseThreshold entries (heavy excavation)
//
// Density is quantised to int16 over [-kDensityClamp, +kDensityClamp] (0.24 mm
// steps). Edits write the *dequantised* value into resident fields, so a
// save/load round trip reproduces bit-identical meshes.
//
// On-disk encoding (per chunk record), designed for 2-4 bytes per edited voxel:
//   varint( (indexDelta << 1) | materialChanged )
//   zigzag-varint( q - previousQ )          // smooth SDF => small deltas
//   [ uint8 material ]                       // only when changed
// followed by a FNV-1a checksum of the whole blob.
//
// Thread safety: reader/writer lock. Workers call applyTo() concurrently under
// a shared lock; the main thread batches edits under one exclusive lock via
// WriteBatch.
// -----------------------------------------------------------------------------

#include "vxe/world/DensityField.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vxe {

struct VoxelOverride {
    float density;
    MaterialId material;
};

inline int16_t quantizeDensity(float d) {
    const float n = std::clamp(d / kDensityClamp, -1.f, 1.f);
    return int16_t(std::lround(n * 32767.f));
}
inline float dequantizeDensity(int16_t q) { return float(q) * (kDensityClamp / 32767.f); }

class ChunkDelta {
public:
    static constexpr uint32_t kOwned = uint32_t(world::kChunkCells * world::kChunkCells * world::kChunkCells);
    static constexpr uint32_t kDenseThreshold = 6000;

    void set(uint16_t localIndex, int16_t q, MaterialId m);
    bool get(uint16_t localIndex, int16_t& q, MaterialId& m) const;
    size_t size() const { return m_dense ? m_denseCount : m_sparse.size(); }
    bool isDense() const { return m_dense; }
    size_t memoryBytes() const;

    template <typename F> // F(uint16_t idx, int16_t q, MaterialId m)
    void forEach(F&& f) const {
        if (!m_dense) {
            for (const auto& e : m_sparse) f(e.idx, e.q, e.mat);
            return;
        }
        for (uint32_t w = 0; w < kOwned / 64; ++w) {
            uint64_t bits = m_mask[w];
            while (bits) {
                const uint32_t b = uint32_t(std::countr_zero(bits));
                const uint16_t i = uint16_t(w * 64 + b);
                f(i, m_denseQ[i], m_denseMat[i]);
                bits &= bits - 1;
            }
        }
    }

    void encode(std::vector<uint8_t>& out) const;
    bool decode(const uint8_t*& p, const uint8_t* end);

private:
    struct Entry { uint16_t idx; int16_t q; MaterialId mat; };
    void promoteToDense();

    bool m_dense = false;
    std::vector<Entry> m_sparse;
    std::vector<uint64_t> m_mask;
    std::vector<int16_t> m_denseQ;
    std::vector<MaterialId> m_denseMat;
    size_t m_denseCount = 0;
};

class DeltaStore {
public:
    // Exclusive batch: all writes in one terraform operation take the lock once.
    class WriteBatch {
    public:
        explicit WriteBatch(DeltaStore& s) : m_store(s), m_lock(s.m_mutex) {}
        // Writes an override and returns the dequantised value actually stored.
        float write(const Vec3i& voxel, float density, MaterialId material);
    private:
        DeltaStore& m_store;
        std::unique_lock<std::shared_mutex> m_lock;
    };

    std::optional<VoxelOverride> read(const Vec3i& voxel) const;

    // O(27) check: does any override land in `key` or its face/edge/corner
    // neighbours at the same LOD? Used by the streamer so procedurally "empty"
    // chunks (open sky) are still built once a player has added terrain there.
    bool hasEditsNear(const ChunkKey& key) const;

    // Applies every override that lands on a sample of `key`'s padded field.
    // Returns the number of samples overridden. Thread-safe (shared lock).
    uint32_t applyTo(const ChunkKey& key, DensityField& field) const;

    std::vector<uint8_t> serialize() const;
    bool deserialize(std::span<const uint8_t> bytes);

    size_t chunkCount() const;
    size_t overrideCount() const;
    size_t memoryBytes() const;
    uint64_t revision() const { std::shared_lock l(m_mutex); return m_revision; }

    static ChunkKey ownerChunk(const Vec3i& voxel) {
        constexpr int32_t n = world::kChunkCells;
        return {floorDiv(voxel.x, n), floorDiv(voxel.y, n), floorDiv(voxel.z, n), 0};
    }
    static uint16_t localIndex(const Vec3i& voxel) {
        constexpr int32_t n = world::kChunkCells;
        return uint16_t(floorMod(voxel.x, n) + n * (floorMod(voxel.y, n) + n * floorMod(voxel.z, n)));
    }

private:
    mutable std::shared_mutex m_mutex;
    void markEdited(const ChunkKey& lod0Owner);

    std::unordered_map<ChunkKey, ChunkDelta, ChunkKeyHash> m_chunks;
    std::unordered_set<ChunkKey, ChunkKeyHash> m_editedNodes;   // owner chunks + all ancestors
    uint64_t m_revision = 0;
};

} // namespace vxe
