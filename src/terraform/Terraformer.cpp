#include "vxe/terraform/Terraformer.h"

#include "vxe/terraform/DeltaStore.h"
#include "vxe/world/ChunkStreamer.h"
#include "vxe/world/DensityGenerator.h"

#include <algorithm>
#include <cmath>

namespace vxe {

namespace {

float smin(float a, float b, float k) {
    if (k <= 0.f) return std::min(a, b);
    const float h = std::max(k - std::abs(a - b), 0.f) / k;
    return std::min(a, b) - h * h * k * 0.25f;
}
float smax(float a, float b, float k) { return -smin(-a, -b, k); }

struct PendingWrite {
    Vec3i voxel;
    float density;
    MaterialId material;
};

} // namespace

Terraformer::Terraformer(ChunkStreamer& streamer, const DensityGenerator& generator, DeltaStore& deltas)
    : m_streamer(streamer), m_generator(generator), m_deltas(deltas) {}

float Terraformer::brushSdf(const TerraformOp& op, const Vec3d& p) {
    const Vec3f d(p - op.center);
    switch (op.shape) {
    case BrushShape::Sphere: return length(d) - op.halfExtents.x;
    case BrushShape::Box: {
        const Vec3f q{std::abs(d.x) - op.halfExtents.x, std::abs(d.y) - op.halfExtents.y, std::abs(d.z) - op.halfExtents.z};
        return length(vmax(q, Vec3f(0.f))) + std::min(std::max({q.x, q.y, q.z}), 0.f);
    }
    case BrushShape::Cylinder: {
        const float radial = std::sqrt(d.x * d.x + d.z * d.z) - op.halfExtents.x;
        const float vertical = std::abs(d.y) - op.halfExtents.y;
        const float ox = std::max(radial, 0.f), oy = std::max(vertical, 0.f);
        return std::sqrt(ox * ox + oy * oy) + std::min(std::max(radial, vertical), 0.f);
    }
    }
    return 1e9f;
}

float Terraformer::currentDensity(const Vec3i& v) const {
    if (auto r = m_streamer.residentDensity(v)) return *r;
    if (auto o = m_deltas.read(v)) return o->density;
    return m_generator.sampleDensity(v);
}

TerraformResult Terraformer::apply(const TerraformOp& op) {
    TerraformResult res;
    const float influence = std::max({op.halfExtents.x, op.halfExtents.y, op.halfExtents.z}) + op.smoothK + 2.f;
    const Vec3d lo = op.center - Vec3d(double(influence));
    const Vec3d hi = op.center + Vec3d(double(influence));
    const Vec3i vlo = floorToInt(lo / double(world::kVoxelSize));
    const Vec3i vhi = floorToInt(hi / double(world::kVoxelSize)) + Vec3i(1);
    const float t = saturate(op.strength);

    // ---- 1+2a: read + combine (no locks held) ------------------------------
    std::vector<PendingWrite> writes;
    writes.reserve(size_t((vhi.x - vlo.x + 1) * (vhi.y - vlo.y + 1) * (vhi.z - vlo.z + 1)) / 2);
    for (int32_t z = vlo.z; z <= vhi.z; ++z)
        for (int32_t y = vlo.y; y <= vhi.y; ++y)
            for (int32_t x = vlo.x; x <= vhi.x; ++x) {
                const Vec3i v{x, y, z};
                const Vec3d p = Vec3d(v) * double(world::kVoxelSize);
                const float b = brushSdf(op, p);
                if (b > op.smoothK + 1.5f) continue;                 // outside the influence band

                const float cur = currentDensity(v);
                float target = cur;
                MaterialId mat = kMatAir;
                if (auto o = m_deltas.read(v)) mat = o->material;
                else if (const DensityField* f = m_streamer.residentField(DeltaStore::ownerChunk(v))) {
                    const Vec3i org = DeltaStore::ownerChunk(v).originVoxel();
                    mat = f->material[size_t(DensityField::pointIndex(x - org.x, y - org.y, z - org.z))];
                } else mat = cur < 0.f ? MaterialId(kMatRock) : MaterialId(kMatAir);

                switch (op.op) {
                case BrushOp::Subtract: target = smax(cur, -b, op.smoothK); break;
                case BrushOp::Add:
                    target = smin(cur, b, op.smoothK);
                    if (target < 0.f && cur >= 0.f) mat = op.material;
                    break;
                case BrushOp::Flatten: {
                    // Inside the brush, blend toward the half-space below center.y.
                    const float plane = float(p.y - op.center.y);
                    const float w = 1.f - smoothstep(-op.smoothK, op.smoothK + 0.5f, b);
                    target = lerp(cur, plane, w);
                    if (target < 0.f && cur >= 0.f) mat = op.material;
                    break;
                }
                case BrushOp::Smooth: {
                    float sum = 0.f;
                    for (int k = 0; k < 6; ++k) {
                        Vec3i n = v;
                        n[k >> 1] += (k & 1) ? 1 : -1;
                        sum += currentDensity(n);
                    }
                    const float w = 1.f - smoothstep(-op.smoothK, op.smoothK + 0.5f, b);
                    target = lerp(cur, sum / 6.f, 0.5f * w);
                    break;
                }
                case BrushOp::Paint:
                    if (b < 0.f && cur < 0.f) mat = op.material;
                    break;
                }
                target = std::clamp(lerp(cur, target, t), -kDensityClamp, kDensityClamp);
                if (target >= 0.f) mat = kMatAir;
                else if (mat == kMatAir) mat = op.material;
                if (std::abs(target - cur) < 1e-4f && op.op != BrushOp::Paint) continue;
                writes.push_back({v, target, mat});
            }
    if (writes.empty()) return res;

    // ---- 2b: commit to the delta store (one exclusive lock) ----------------
    {
        DeltaStore::WriteBatch batch(m_deltas);
        for (PendingWrite& w : writes) w.density = batch.write(w.voxel, w.density, w.material);  // dequantised
    }
    res.voxelsWritten = uint32_t(writes.size());

    // ---- 3: patch resident LOD0 fields (owner + every neighbour's padding) --
    constexpr int32_t n = world::kChunkCells;
    Vec3i wlo(INT32_MAX), whi(INT32_MIN);
    for (const auto& w : writes) { wlo = vmin(wlo, w.voxel); whi = vmax(whi, w.voxel); }
    // A field covers local points [-kFieldOffset, hiPad]: chunk c sees voxel v if
    // c*n - kFieldOffset <= v <= c*n + hiPad.
    constexpr int32_t loPad = -kFieldOffset, hiPad = kFieldSize - 1 - kFieldOffset;
    const Vec3i c0{floorDiv(wlo.x - hiPad, n), floorDiv(wlo.y - hiPad, n), floorDiv(wlo.z - hiPad, n)};
    const Vec3i c1{floorDiv(whi.x - loPad, n), floorDiv(whi.y - loPad, n), floorDiv(whi.z - loPad, n)};
    for (int32_t cz = c0.z; cz <= c1.z; ++cz)
        for (int32_t cy = c0.y; cy <= c1.y; ++cy)
            for (int32_t cx = c0.x; cx <= c1.x; ++cx) {
                const ChunkKey key{cx, cy, cz, 0};
                DensityField* f = m_streamer.residentField(key);
                if (!f) continue;
                const Vec3i org = key.originVoxel();
                for (const auto& w : writes) {
                    const Vec3i l = w.voxel - org;
                    if (l.x < loPad || l.y < loPad || l.z < loPad || l.x > hiPad || l.y > hiPad || l.z > hiPad) continue;
                    const size_t i = size_t(DensityField::pointIndex(l.x, l.y, l.z));
                    f->density[i] = w.density;
                    f->material[i] = w.material;
                    ++res.residentSamplesPatched;
                }
            }

    // ---- 4+5: rebuild + notify ----------------------------------------------
    res.affected = {Vec3d(wlo) * double(world::kVoxelSize), Vec3d(whi) * double(world::kVoxelSize)};
    m_streamer.invalidateRegion(res.affected, TaskPriority::Critical);
    for (auto* l : m_listeners) l->onTerrainEdited(res.affected);
    return res;
}

} // namespace vxe
