#include "vxe/terraform/DeltaStore.h"

#include <cstring>
#include <mutex>

namespace vxe {

namespace {

void putVarint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) { out.push_back(uint8_t(v | 0x80)); v >>= 7; }
    out.push_back(uint8_t(v));
}
bool getVarint(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (p >= end) return false;
        const uint8_t b = *p++;
        v |= uint64_t(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}
uint32_t zigzag(int32_t v) { return (uint32_t(v) << 1) ^ uint32_t(v >> 31); }
int32_t unzigzag(uint32_t v) { return int32_t(v >> 1) ^ -int32_t(v & 1); }

void putI32(std::vector<uint8_t>& out, int32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(uint32_t(v) >> (8 * i)));
}
bool getI32(const uint8_t*& p, const uint8_t* end, int32_t& v) {
    if (end - p < 4) return false;
    uint32_t u = 0;
    for (int i = 0; i < 4; ++i) u |= uint32_t(p[i]) << (8 * i);
    p += 4;
    v = int32_t(u);
    return true;
}
uint32_t fnv1a(const uint8_t* data, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= data[i]; h *= 16777619u; }
    return h;
}

constexpr uint32_t kMagic = 0x31445856; // "VXD1"
constexpr uint32_t kVersion = 1;

} // namespace

// ----------------------------- ChunkDelta -----------------------------------

void ChunkDelta::set(uint16_t idx, int16_t q, MaterialId m) {
    if (m_dense) {
        uint64_t& word = m_mask[idx >> 6];
        const uint64_t bit = 1ull << (idx & 63);
        if (!(word & bit)) { word |= bit; ++m_denseCount; }
        m_denseQ[idx] = q;
        m_denseMat[idx] = m;
        return;
    }
    auto it = std::lower_bound(m_sparse.begin(), m_sparse.end(), idx, [](const Entry& e, uint16_t i) { return e.idx < i; });
    if (it != m_sparse.end() && it->idx == idx) { it->q = q; it->mat = m; return; }
    m_sparse.insert(it, Entry{idx, q, m});
    if (m_sparse.size() > kDenseThreshold) promoteToDense();
}

bool ChunkDelta::get(uint16_t idx, int16_t& q, MaterialId& m) const {
    if (m_dense) {
        if (!(m_mask[idx >> 6] & (1ull << (idx & 63)))) return false;
        q = m_denseQ[idx]; m = m_denseMat[idx];
        return true;
    }
    auto it = std::lower_bound(m_sparse.begin(), m_sparse.end(), idx, [](const Entry& e, uint16_t i) { return e.idx < i; });
    if (it == m_sparse.end() || it->idx != idx) return false;
    q = it->q; m = it->mat;
    return true;
}

void ChunkDelta::promoteToDense() {
    m_mask.assign(kOwned / 64, 0);
    m_denseQ.assign(kOwned, 0);
    m_denseMat.assign(kOwned, 0);
    m_denseCount = m_sparse.size();
    for (const auto& e : m_sparse) {
        m_mask[e.idx >> 6] |= 1ull << (e.idx & 63);
        m_denseQ[e.idx] = e.q;
        m_denseMat[e.idx] = e.mat;
    }
    m_sparse.clear();
    m_sparse.shrink_to_fit();
    m_dense = true;
}

size_t ChunkDelta::memoryBytes() const {
    return m_dense ? m_mask.size() * 8 + m_denseQ.size() * 2 + m_denseMat.size() : m_sparse.capacity() * sizeof(Entry);
}

void ChunkDelta::encode(std::vector<uint8_t>& out) const {
    putVarint(out, size());
    int32_t prevIdx = -1, prevQ = 0;
    int prevMat = -1;
    forEach([&](uint16_t idx, int16_t q, MaterialId m) {
        const bool matChanged = int(m) != prevMat;
        putVarint(out, (uint64_t(int32_t(idx) - prevIdx - 1) << 1) | (matChanged ? 1u : 0u));
        putVarint(out, zigzag(int32_t(q) - prevQ));
        if (matChanged) out.push_back(m);
        prevIdx = idx; prevQ = q; prevMat = m;
    });
}

bool ChunkDelta::decode(const uint8_t*& p, const uint8_t* end) {
    *this = ChunkDelta{};
    uint64_t count = 0;
    if (!getVarint(p, end, count) || count > kOwned) return false;
    int32_t prevIdx = -1, prevQ = 0;
    MaterialId mat = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t head = 0, dq = 0;
        if (!getVarint(p, end, head) || !getVarint(p, end, dq)) return false;
        const int32_t idx = prevIdx + 1 + int32_t(head >> 1);
        if (idx < 0 || uint32_t(idx) >= kOwned) return false;
        if (head & 1) { if (p >= end) return false; mat = *p++; }
        const int32_t q = prevQ + unzigzag(uint32_t(dq));
        if (q < -32768 || q > 32767) return false;
        set(uint16_t(idx), int16_t(q), mat);
        prevIdx = idx; prevQ = q;
    }
    return true;
}

// ----------------------------- DeltaStore -----------------------------------

float DeltaStore::WriteBatch::write(const Vec3i& voxel, float density, MaterialId material) {
    const int16_t q = quantizeDensity(density);
    const ChunkKey owner = ownerChunk(voxel);
    auto [it, inserted] = m_store.m_chunks.try_emplace(owner);
    if (inserted) m_store.markEdited(owner);
    it->second.set(localIndex(voxel), q, material);
    ++m_store.m_revision;
    return dequantizeDensity(q);
}

void DeltaStore::markEdited(const ChunkKey& owner) {
    ChunkKey k = owner;
    for (int l = 0; l <= world::kMaxLod; ++l) {
        m_editedNodes.insert(k);
        k = k.parent();
    }
}

bool DeltaStore::hasEditsNear(const ChunkKey& key) const {
    std::shared_lock lock(m_mutex);
    if (m_editedNodes.empty()) return false;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (m_editedNodes.count(ChunkKey{key.x + dx, key.y + dy, key.z + dz, key.lod})) return true;
    return false;
}

std::optional<VoxelOverride> DeltaStore::read(const Vec3i& voxel) const {
    std::shared_lock lock(m_mutex);
    auto it = m_chunks.find(ownerChunk(voxel));
    if (it == m_chunks.end()) return std::nullopt;
    int16_t q; MaterialId m;
    if (!it->second.get(localIndex(voxel), q, m)) return std::nullopt;
    return VoxelOverride{dequantizeDensity(q), m};
}

uint32_t DeltaStore::applyTo(const ChunkKey& key, DensityField& field) const {
    constexpr int32_t n = world::kChunkCells;
    const int32_t s = key.strideVoxels();
    const Vec3i origin = key.originVoxel();
    // Padded sample range in LOD0 voxels: sample i <-> origin + (i - kFieldOffset) * s
    const Vec3i lo = origin - Vec3i(kFieldOffset * s);
    const Vec3i hi = origin + Vec3i((kFieldSize - 1 - kFieldOffset) * s);

    uint32_t applied = 0;
    std::shared_lock lock(m_mutex);
    if (m_chunks.empty()) return 0;

    const Vec3i c0{floorDiv(lo.x, n), floorDiv(lo.y, n), floorDiv(lo.z, n)};
    const Vec3i c1{floorDiv(hi.x, n), floorDiv(hi.y, n), floorDiv(hi.z, n)};

    // Walks every override of every edited LOD0 chunk overlapping the padded
    // field, handing each one to `sink` as (voxel, density, material).
    const auto forEachOverride = [&](auto&& sink) {
        for (int32_t cz = c0.z; cz <= c1.z; ++cz)
            for (int32_t cy = c0.y; cy <= c1.y; ++cy)
                for (int32_t cx = c0.x; cx <= c1.x; ++cx) {
                    auto it = m_chunks.find(ChunkKey{cx, cy, cz, 0});
                    if (it == m_chunks.end()) continue;
                    const Vec3i base{cx * n, cy * n, cz * n};
                    it->second.forEach([&](uint16_t idx, int16_t q, MaterialId m) {
                        sink(base + Vec3i(idx % n, (idx / n) % n, idx / (n * n)), dequantizeDensity(q), m);
                    });
                }
    };

    // ---- LOD0: one override, one sample. Exact, and must stay exact: edits
    // are stored relative to regenerated density, so a save/load round trip has
    // to reproduce bit-identical fields.
    if (s == 1) {
        forEachOverride([&](const Vec3i& v, float d, MaterialId m) {
            const Vec3i rel = v - lo;
            if (rel.x < 0 || rel.y < 0 || rel.z < 0) return;
            if (rel.x >= kFieldSize || rel.y >= kFieldSize || rel.z >= kFieldSize) return;
            const size_t fi = size_t(DensityField::index(rel.x, rel.y, rel.z));
            field.density[fi] = d;
            field.material[fi] = m;
            ++applied;
        });
        return applied;
    }

    // ---- Coarse LODs: reduce each s^3 block onto the sample it covers.
    struct Bin {
        int32_t sample;        // flat index into the padded field
        float weighted;        // SUM w_i * density_i
        float weight;          // SUM w_i
        int32_t count;         // edited voxels in this block, <= s^3
        float mostSolid;       // lowest density seen, for choosing a material
        MaterialId material;
    };
    std::vector<Bin> bins;
    const float airWeight = m_airWeight;
    const int32_t half = s / 2;

    forEachOverride([&](const Vec3i& v, float d, MaterialId m) {
        // The sample a voxel belongs to is the nearest one, so each block is
        // centred on its sample rather than hanging off one corner.
        const Vec3i rel = v - lo;
        const Vec3i si{floorDiv(rel.x + half, s), floorDiv(rel.y + half, s), floorDiv(rel.z + half, s)};
        if (si.x < 0 || si.y < 0 || si.z < 0) return;
        if (si.x >= kFieldSize || si.y >= kFieldSize || si.z >= kFieldSize) return;
        const float w = d >= 0.f ? airWeight : 1.f;
        bins.push_back({DensityField::index(si.x, si.y, si.z), w * d, w, 1, d, m});
    });
    if (bins.empty()) return 0;

    std::sort(bins.begin(), bins.end(), [](const Bin& a, const Bin& b) { return a.sample < b.sample; });

    const float blockVoxels = float(s) * float(s) * float(s);
    for (size_t i = 0; i < bins.size();) {
        size_t j = i;
        Bin acc = bins[i];
        for (++j; j < bins.size() && bins[j].sample == acc.sample; ++j) {
            acc.weighted += bins[j].weighted;
            acc.weight += bins[j].weight;
            acc.count += bins[j].count;
            if (bins[j].mostSolid < acc.mostSolid) {
                acc.mostSolid = bins[j].mostSolid;
                acc.material = bins[j].material;
            }
        }
        // Voxels nobody edited keep the procedural value already in the field,
        // at unit weight -- airWeight biases the edits, not the rock around them.
        const size_t fi = size_t(acc.sample);
        const float untouched = std::max(blockVoxels - float(acc.count), 0.f);
        const float reduced = (acc.weighted + untouched * field.density[fi]) / (acc.weight + untouched);
        field.density[fi] = std::clamp(reduced, -kDensityClamp, kDensityClamp);
        field.material[fi] = reduced >= 0.f ? MaterialId(kMatAir) : acc.material;
        ++applied;
        i = j;
    }
    return applied;
}

std::vector<uint8_t> DeltaStore::serialize() const {
    std::shared_lock lock(m_mutex);
    std::vector<uint8_t> out;
    putI32(out, int32_t(kMagic));
    putI32(out, int32_t(kVersion));
    putI32(out, int32_t(m_chunks.size()));
    for (const auto& [key, delta] : m_chunks) {
        putI32(out, key.x); putI32(out, key.y); putI32(out, key.z);
        delta.encode(out);
    }
    putI32(out, int32_t(fnv1a(out.data(), out.size())));
    return out;
}

bool DeltaStore::deserialize(std::span<const uint8_t> bytes) {
    if (bytes.size() < 16) return false;
    const uint8_t* p = bytes.data();
    const uint8_t* end = bytes.data() + bytes.size() - 4;
    int32_t magic, version, count, checksum;
    const uint8_t* cp = end;
    getI32(cp, bytes.data() + bytes.size(), checksum);
    if (uint32_t(checksum) != fnv1a(bytes.data(), bytes.size() - 4)) return false;
    if (!getI32(p, end, magic) || uint32_t(magic) != kMagic) return false;
    if (!getI32(p, end, version) || uint32_t(version) != kVersion) return false;
    if (!getI32(p, end, count) || count < 0) return false;

    std::unordered_map<ChunkKey, ChunkDelta, ChunkKeyHash> chunks;
    chunks.reserve(size_t(count));
    for (int32_t i = 0; i < count; ++i) {
        ChunkKey k{};
        if (!getI32(p, end, k.x) || !getI32(p, end, k.y) || !getI32(p, end, k.z)) return false;
        ChunkDelta d;
        if (!d.decode(p, end)) return false;
        chunks.emplace(k, std::move(d));
    }
    std::unique_lock lock(m_mutex);
    m_chunks = std::move(chunks);
    m_editedNodes.clear();
    for (const auto& [k, d] : m_chunks) markEdited(k);
    ++m_revision;
    return true;
}

size_t DeltaStore::chunkCount() const { std::shared_lock l(m_mutex); return m_chunks.size(); }
size_t DeltaStore::overrideCount() const {
    std::shared_lock l(m_mutex);
    size_t n = 0;
    for (const auto& [k, d] : m_chunks) n += d.size();
    return n;
}
size_t DeltaStore::memoryBytes() const {
    std::shared_lock l(m_mutex);
    size_t n = 0;
    for (const auto& [k, d] : m_chunks) n += d.memoryBytes() + sizeof(ChunkKey);
    return n;
}

} // namespace vxe
