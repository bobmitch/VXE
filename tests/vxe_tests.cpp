// Self-contained unit/integration tests (no framework dependency).
#include "vxe/building/BuildingSystem.h"
#include "vxe/core/Memory.h"
#include "vxe/core/TaskSystem.h"
#include "vxe/meshing/DualContouring.h"
#include "vxe/meshing/MeshOptimizerStage.h"
#include "vxe/render/GpuAllocators.h"
#include "vxe/render/MaterialSystem.h"
#include "vxe/render/TerrainRenderer.h"
#include "vxe/terraform/DeltaStore.h"
#include "vxe/terraform/Terraformer.h"
#include "vxe/world/ChunkStreamer.h"
#include "vxe/world/DensityGenerator.h"
#if VXE_WITH_JOLT
#include "vxe/physics/PhysicsWorld.h"
#include "vxe/physics/TerrainCollisionBaker.h"
#endif
#if VXE_WITH_OZZ
#include "vxe/animation/CharacterAnimator.h"
#endif

#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>

using namespace vxe;

namespace {
int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        if (!(cond)) { ++g_failures; std::printf("  FAILED %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

struct Test { const char* name; std::function<void()> fn; };
std::vector<Test>& registry() { static std::vector<Test> t; return t; }
struct Reg { Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
#define TEST(name) static void name(); static Reg reg_##name(#name, name); static void name()

std::unique_ptr<DensityField> sdfField(const std::function<float(Vec3f)>& f) {
    auto field = std::make_unique<DensityField>();
    for (int z = 0; z < kFieldSize; ++z)
        for (int y = 0; y < kFieldSize; ++y)
            for (int x = 0; x < kFieldSize; ++x) {
                const size_t i = size_t(DensityField::index(x, y, z));
                field->density[i] = std::clamp(f(Vec3f(float(x - kFieldOffset), float(y - kFieldOffset), float(z - kFieldOffset))), -kDensityClamp, kDensityClamp);
                field->material[i] = kMatRock;
            }
    return field;
}

void pumpUntilIdle(ChunkStreamer& s, const Vec3d& focus, int maxFrames = 20000) {
    for (int i = 0; i < maxFrames; ++i) {
        s.update(focus);
        if (s.isIdle()) return;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}
} // namespace

// ---------------------------------------------------------------------------
TEST(task_graph_dependencies_and_parallel_for) {
    TaskSystem ts(3);
    std::atomic<int> step{0};
    int a = -1, b = -1, c = -1;
    auto ta = ts.submit([&] { std::this_thread::sleep_for(std::chrono::milliseconds(5)); a = step++; });
    auto tb = ts.submit([&] { b = step++; }, TaskPriority::Normal, {ta});
    auto tc = ts.submit([&] { c = step++; }, TaskPriority::Critical, {ta, tb});
    ts.wait(tc);
    CHECK(a == 0 && b == 1 && c == 2);

    std::atomic<uint64_t> sum{0};
    ts.parallelFor(10000, 97, [&](uint32_t s, uint32_t e) { for (uint32_t i = s; i < e; ++i) sum += i; });
    CHECK(sum == 10000ull * 9999ull / 2ull);
}

TEST(block_pool_backpressure) {
    BlockPool<DensityField> pool(2, 2);
    std::vector<PoolPtr<DensityField>> held;
    for (int i = 0; i < 4; ++i) held.push_back(pool.acquire());
    CHECK(pool.liveBlocks() == 4);
    CHECK(!pool.acquire());                    // capacity exhausted -> nullptr
    held.pop_back();
    CHECK(pool.acquire() != nullptr);
}

TEST(dual_contouring_closed_manifold_and_winding) {
    const Vec3f c(16.3f, 15.7f, 16.1f);
    auto f = sdfField([&](Vec3f p) { return length(p - c) - 10.f; });
    ChunkMesh m;
    contourChunk(*f, 1.f, {}, m);
    CHECK(m.triangleCount() > 1000);
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    int outward = 0;
    for (size_t t = 0; t < m.indices.size(); t += 3) {
        for (int k = 0; k < 3; ++k) edges[{m.indices[t + k], m.indices[t + (k + 1) % 3]}]++;
        const Vec3f A = m.vertices[m.indices[t]].position, B = m.vertices[m.indices[t + 1]].position,
                    C = m.vertices[m.indices[t + 2]].position;
        if (dot(cross(B - A, C - A), (A + B + C) / 3.f - c) > 0.f) ++outward;
    }
    int bad = 0;
    for (auto& [e, n] : edges) if (n != 1 || !edges.count({e.second, e.first})) ++bad;
    CHECK(bad == 0);                                        // every directed edge has exactly one twin
    CHECK(outward == int(m.triangleCount()));               // CCW = outward
    double err = 0;
    for (auto& v : m.vertices) err = std::max(err, double(std::abs(length(v.position - c) - 10.f)));
    std::printf("    sphere r=10: max radial error %.4f m\n", err);
    CHECK(err < 0.1);
}

TEST(dual_contouring_beats_surface_nets_on_creases) {
    const Vec3f c(16.3f, 15.7f, 16.1f);
    auto sdf = [&](Vec3f p) { Vec3f d = p - c; Vec3f q{std::abs(d.x) - 9, std::abs(d.y) - 9, std::abs(d.z) - 9};
                              return length(vmax(q, Vec3f(0.f))) + std::min(std::max({q.x, q.y, q.z}), 0.f); };
    auto f = sdfField(sdf);
    double err[2];
    for (int k = 0; k < 2; ++k) {
        MesherSettings s; s.sharpness = k == 0 ? 1.f : 0.f;
        ChunkMesh m; contourChunk(*f, 1.f, s, m);
        double sum = 0; for (auto& v : m.vertices) sum += std::abs(sdf(v.position));
        err[k] = sum / double(m.vertices.size());
    }
    std::printf("    mean |sdf| DC=%.4f  SurfaceNets=%.4f\n", err[0], err[1]);
    CHECK(err[0] < err[1] * 0.5);
}

TEST(chunk_seams_are_watertight) {
    DensityGenerator gen;
    const float h = gen.sampleHeight(48.f, 16.f);
    const int cy = int(std::floor(h / 32.f));
    const ChunkKey a{1, cy, 0, 0}, b{2, cy, 0, 0};
    auto fa = std::make_unique<DensityField>(), fb = std::make_unique<DensityField>();
    gen.generate(a, *fa);
    gen.generate(b, *fb);
    ChunkMesh ma, mb;
    contourChunk(*fa, 1.f, {}, ma);
    contourChunk(*fb, 1.f, {}, mb);
    // Vertices in A's last cell column (x in [32,33)) must exist identically in B's first column.
    std::vector<Vec3d> bPos;
    for (auto& v : mb.vertices) if (v.position.x < 1.f) bPos.push_back(b.originWorld() + Vec3d(v.position));
    int shared = 0, matched = 0;
    for (auto& v : ma.vertices) {
        if (v.position.x < 32.f) continue;
        ++shared;
        const Vec3d w = a.originWorld() + Vec3d(v.position);
        for (auto& p : bPos) if (length(p - w) < 1e-4) { ++matched; break; }
    }
    std::printf("    shared boundary vertices: %d, matched: %d\n", shared, matched);
    CHECK(shared > 0);
    CHECK(shared == matched);
}

// Squared distance from a point to a triangle (Ericson, Real-Time Collision
// Detection 5.1.5) - a nearest-vertex probe would report up to half the coarse
// cell size as "gap" even for a perfect match.
static double pointTriangleDist2(const Vec3d& p, const Vec3d& a, const Vec3d& b, const Vec3d& c) {
    const Vec3d ab = b - a, ac = c - a, ap = p - a;
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return dot(ap, ap);
    const Vec3d bp = p - b;
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return dot(bp, bp);
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { const Vec3d q = ap - ab * (d1 / (d1 - d3)); return dot(q, q); }
    const Vec3d cp = p - c;
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return dot(cp, cp);
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { const Vec3d q = ap - ac * (d2 / (d2 - d6)); return dot(q, q); }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const Vec3d q = bp - (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
        return dot(q, q);
    }
    const double denom = 1.0 / (va + vb + vc);
    const Vec3d q = ap - (ab * (vb * denom) + ac * (vc * denom));
    return dot(q, q) ;
}

// Fills a field by sampling a world-space SDF on the chunk's own lattice:
// sample index s is origin + (s - kFieldOffset) * step.
static std::unique_ptr<DensityField> sdfFieldAt(const Vec3d& origin, float step,
                                                const std::function<float(Vec3d)>& f) {
    auto field = std::make_unique<DensityField>();
    for (int z = 0; z < kFieldSize; ++z)
        for (int y = 0; y < kFieldSize; ++y)
            for (int x = 0; x < kFieldSize; ++x) {
                const Vec3d w = origin + Vec3d(double(x - kFieldOffset), double(y - kFieldOffset),
                                               double(z - kFieldOffset)) * double(step);
                const size_t i = size_t(DensityField::index(x, y, z));
                field->density[i] = std::clamp(f(w), -kDensityClamp, kDensityClamp);
                field->material[i] = kMatRock;
            }
    return field;
}

// Worst distance from `from`'s vertices near the seam to `to`'s *surface*. With
// a watertight join this is 0; with a crack it is how far a skirt has to reach
// to cover it. Restricted to the strip where both chunks actually have surface,
// so chunk corners and the far perimeter do not pollute the number.
// `box` is the region both chunks cover: a coarse chunk is twice the size of a
// fine one, so its vertices outside the fine chunk's own window have no
// counterpart *here* (another fine chunk owns them) and would otherwise be
// counted as enormous gaps.
static double gapsToSurface(const ChunkMesh& from, const Vec3d& fromOrigin, const ChunkMesh& to, const Vec3d& toOrigin,
                            double planeX, double band, const Aabbd& box, std::vector<double>& out) {
    const size_t targetCount = to.surfaceIndexCount();
    double worst = 0.0;
    for (const auto& v : from.vertices) {
        const Vec3d p = fromOrigin + Vec3d(v.position);
        if (std::abs(p.x - planeX) > band || !box.contains({planeX, p.y, p.z})) continue;
        double best = 1e30;
        for (size_t t = 0; t + 2 < targetCount; t += 3) {
            const Vec3d a = toOrigin + Vec3d(to.vertices[to.indices[t]].position);
            const Vec3d b = toOrigin + Vec3d(to.vertices[to.indices[t + 1]].position);
            const Vec3d c = toOrigin + Vec3d(to.vertices[to.indices[t + 2]].position);
            best = std::min(best, pointTriangleDist2(p, a, b, c));
            if (best < 1e-12) break;
        }
        if (best < 1e29) { out.push_back(std::sqrt(best)); worst = std::max(worst, std::sqrt(best)); }
    }
    return worst;
}

static double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    const size_t k = std::min(v.size() - 1, size_t(q * double(v.size())));
    std::nth_element(v.begin(), v.begin() + ptrdiff_t(k), v.end());
    return v[k];
}

TEST(lod_seam_cracks_are_covered_by_skirts) {
    // A stride-1 chunk and a stride-2 chunk meeting on the plane x = 64. Both
    // sample the same lattice, but the coarse one only sees every other point,
    // so it smooths away detail the fine one resolves. Four fine cells face one
    // coarse cell, there is no edge both sides agree to emit a quad for, and the
    // two surfaces just end near each other.
    const auto terrain = [](Vec3d p) {
        return float(p.y - (16.0 + 3.0 * std::sin(p.x * 0.25) + 2.0 * std::cos(p.z * 0.31) +
                            1.5 * std::sin(p.x * 0.7 + p.z * 0.5)));
    };
    const Vec3d fineOrigin{32, 0, 0}, coarseOrigin{64, 0, 0};
    const double planeX = 64.0;
    const Aabbd shared{{0, 3, 3}, {128, 29, 29}};   // the fine chunk's interior, in y and z
    auto ff = sdfFieldAt(fineOrigin, 1.f, terrain);
    auto cf = sdfFieldAt(coarseOrigin, 2.f, terrain);

    MesherSettings bare;                      // skirtDepthCells == 0
    ChunkMesh fm, cm;
    contourChunk(*ff, 1.f, bare, fm);
    contourChunk(*cf, 2.f, bare, cm);
    CHECK(!fm.empty() && !cm.empty());

    std::vector<double> synth;
    const double gapFine = gapsToSurface(fm, fineOrigin, cm, coarseOrigin, planeX, 1.5, shared, synth);
    const double gapCoarse = gapsToSurface(cm, coarseOrigin, fm, fineOrigin, planeX, 2.5, shared, synth);
    const double gap = std::max(gapFine, gapCoarse);
    std::printf("    unskirted stride1|stride2 seam: worst gap %.3f m (fine->coarse %.3f, coarse->fine %.3f)\n", gap,
                gapFine, gapCoarse);
    CHECK(gap > 0.02);                        // the crack this fix exists for is real

    // Each side's collar scales with its own stride, so the deep one comes from
    // the coarse side - which is also the side that got the geometry wrong.
    MesherSettings skirted = bare;
    skirted.skirtDepthCells = StreamerConfig{}.mesherLod0.skirtDepthCells;   // validate what actually ships
    CHECK(skirted.skirtDepthCells > 0.f);
    ChunkMesh fs, cs;
    const MesherStats fstats = contourChunk(*ff, 1.f, skirted, fs);
    const MesherStats cstats = contourChunk(*cf, 2.f, skirted, cs);
    const double reach = double(skirted.skirtDepthCells) * 2.0;   // coarse side, 2 m cells
    std::printf("    skirts: fine %u tris over %u open edges, coarse %u tris; reach %.1f m\n", fstats.skirtTriangles,
                fstats.openEdges, cstats.skirtTriangles, reach);
    CHECK(fstats.skirtTriangles > 0 && cstats.skirtTriangles > 0);
    CHECK(reach > gap);
    // Cost: the collar is proportional to the surface perimeter, not its area.
    std::printf("    skirt overhead: fine %.1f%%, coarse %.1f%% more triangles\n",
                100.0 * double(fstats.skirtTriangles) / double(fm.triangleCount()),
                100.0 * double(cstats.skirtTriangles) / double(cm.triangleCount()));

    // The surface prefix must be untouched, so collision and the unskirted path
    // see exactly what they saw before.
    CHECK(fs.skirtIndexStart == fm.indices.size());
    CHECK(std::equal(fm.indices.begin(), fm.indices.end(), fs.indices.begin()));
    CHECK(fs.indices.size() == fs.skirtIndexStart + size_t(fstats.skirtTriangles) * 3);

    // Every open edge gets exactly one collar quad, and no interior edge does.
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (size_t t = 0; t < fm.indices.size(); t += 3)
        for (int k = 0; k < 3; ++k) edges[{fm.indices[t + k], fm.indices[t + (k + 1) % 3]}]++;
    uint32_t open = 0;
    for (auto& [e, n] : edges) if (n == 1 && !edges.count({e.second, e.first})) ++open;
    CHECK(open == fstats.openEdges);
    CHECK(fstats.skirtTriangles == open * 2);

    // Each collar quad is {a, a', b', a, b', b}, with v' straight back along
    // -normal by at most the configured depth.
    const float depth = skirted.skirtDepthCells * 1.f;
    int misplaced = 0, backfacing = 0;
    for (size_t t = fs.skirtIndexStart; t + 5 < fs.indices.size(); t += 6) {
        const TerrainVertex& a = fs.vertices[fs.indices[t]];
        const TerrainVertex& a2 = fs.vertices[fs.indices[t + 1]];
        const TerrainVertex& b2 = fs.vertices[fs.indices[t + 2]];
        const TerrainVertex& b = fs.vertices[fs.indices[t + 5]];
        const TerrainVertex* tops[2] = {&a, &b};
        const TerrainVertex* bots[2] = {&a2, &b2};
        for (int k = 0; k < 2; ++k) {
            const Vec3f back = tops[k]->position - bots[k]->position;
            const float along = dot(back, tops[k]->normal);
            if (along < -1e-4f || along > depth + 1e-3f) ++misplaced;              // wrong way, or too far
            if (length(back - tops[k]->normal * along) > 1e-3f) ++misplaced;       // not parallel to -normal
        }
        const Vec3f n = cross(a2.position - a.position, b2.position - a.position);
        if (dot(n, a.normal) > 1e-4f) ++backfacing;   // must face away from the surface, not along it
    }
    CHECK(misplaced == 0);
    CHECK(backfacing == 0);

    // The invariant that keeps the collar invisible: it is buried. A collar
    // vertex that ends up in the air is geometry hanging over the terrain.
    // Each one is clipped to the solid, so none of them can be.
    auto densityAt = [](const DensityField& f, Vec3f local, float step) {
        const Vec3f p = vclamp(local / step, Vec3f(float(-kFieldOffset)), Vec3f(float(kFieldSize - kFieldOffset - 2)));
        const int32_t x = int32_t(std::floor(p.x)), y = int32_t(std::floor(p.y)), z = int32_t(std::floor(p.z));
        const float fx = p.x - float(x), fy = p.y - float(y), fz = p.z - float(z);
        const float c00 = lerp(f.at(x, y, z), f.at(x + 1, y, z), fx);
        const float c10 = lerp(f.at(x, y + 1, z), f.at(x + 1, y + 1, z), fx);
        const float c01 = lerp(f.at(x, y, z + 1), f.at(x + 1, y, z + 1), fx);
        const float c11 = lerp(f.at(x, y + 1, z + 1), f.at(x + 1, y + 1, z + 1), fx);
        return lerp(lerp(c00, c10, fy), lerp(c01, c11, fy), fz);
    };
    int inAir = 0, degenerate = 0;
    float worstDensity = -1e30f;
    for (size_t i = fm.vertices.size(); i < fs.vertices.size(); ++i) {
        const float dens = densityAt(*ff, fs.vertices[i].position, 1.f);
        worstDensity = std::max(worstDensity, dens);
        if (dens > 1e-3f) ++inAir;
    }
    for (size_t t = fs.skirtIndexStart; t + 5 < fs.indices.size(); t += 6)
        if (length(fs.vertices[fs.indices[t]].position - fs.vertices[fs.indices[t + 1]].position) < 1e-4f) ++degenerate;
    std::printf("    collar: %zu vertices, worst density %+.3f (<0 = inside), %d in air, %d quads fully clipped\n",
                fs.vertices.size() - fm.vertices.size(), double(worstDensity), inAir, degenerate);
    CHECK(inAir == 0);

    // Same measurement on real generator terrain, which has ridges and cave
    // overhangs the synthetic field above does not. A LOD1 chunk and the LOD0
    // chunk against its -x face: chunk {2cx-1, *, *, 0} spans x up to 64*cx,
    // which is exactly where {cx, *, *, 1} starts.
    DensityGenerator gen;
    int sampled = 0;
    double worstReal = 0.0;
    std::vector<double> realFine, realCoarse;
    for (int cx = 2; cx <= 9 && sampled < 8; ++cx)
        for (int cz = 1; cz <= 6 && sampled < 8; ++cz) {
            const double seamX = 64.0 * cx;
            const float h = gen.sampleHeight(float(seamX - 8.0), float(64.0 * cz + 16.0));
            if (!(h > 8.f && h < 400.f)) continue;
            const ChunkKey lod1{cx, int32_t(std::floor(double(h) / 64.0)), cz, 1};
            const ChunkKey lod0{2 * cx - 1, int32_t(std::floor(double(h) / 32.0)), 2 * cz, 0};
            auto f0 = std::make_unique<DensityField>(), f1 = std::make_unique<DensityField>();
            gen.generate(lod0, *f0);
            gen.generate(lod1, *f1);
            ChunkMesh m0, m1;
            contourChunk(*f0, 1.f, bare, m0);
            contourChunk(*f1, 2.f, bare, m1);
            if (m0.empty() || m1.empty()) continue;
            const Vec3d o0 = lod0.originWorld(), o1 = lod1.originWorld();
            // Only where the LOD0 chunk actually reaches: it is a 32 m cube
            // inside the LOD1 chunk's 64 m one.
            const Aabbd overlap{o0 + Vec3d(0, 3, 3), o0 + Vec3d(32, 29, 29)};
            worstReal = std::max({worstReal, gapsToSurface(m0, o0, m1, o1, seamX, 1.5, overlap, realFine),
                                  gapsToSurface(m1, o1, m0, o0, seamX, 2.5, overlap, realCoarse)});
            ++sampled;
        }
    // Each collar scales with its own stride, so the two directions have to be
    // judged against different budgets: the LOD0 rim only reaches
    // skirtDepthCells * 1 m, the LOD1 rim twice that.
    std::printf("    generator terrain, %d LOD0|LOD1 seams:\n", sampled);
    std::printf("      LOD0 rim -> LOD1 surface (%zu pts): p50 %.2f  p90 %.2f  p99 %.2f  max %.2f m\n",
                realFine.size(), percentile(realFine, 0.50), percentile(realFine, 0.90), percentile(realFine, 0.99),
                percentile(realFine, 1.0));
    std::printf("      LOD1 rim -> LOD0 surface (%zu pts): p50 %.2f  p90 %.2f  p99 %.2f  max %.2f m\n",
                realCoarse.size(), percentile(realCoarse, 0.50), percentile(realCoarse, 0.90),
                percentile(realCoarse, 0.99), percentile(realCoarse, 1.0));
    CHECK(sampled > 0);
    CHECK(worstReal > 0.1);
    // The collar is sized for the geometric deviation, not for the rare case
    // where the coarse LOD misses a feature outright (a 4 m cave tunnel is badly
    // undersampled at a 2 m stride, so its mouth can simply be absent on one
    // side). That is a density-LOD mismatch, not a seam crack, and no skirt
    // depth that is sane for the other 99% of the seam would hide it.
    std::printf("      reach: LOD0 rim %.1f m, LOD1 rim %.1f m\n", double(skirted.skirtDepthCells) * 1.0,
                double(skirted.skirtDepthCells) * 2.0);
    CHECK(skirted.skirtDepthCells * 1.f > percentile(realFine, 0.99));
    CHECK(skirted.skirtDepthCells * 2.f > percentile(realCoarse, 0.99));
}

TEST(delta_store_roundtrip_and_compression) {
    DeltaStore s;
    std::mt19937 rng(7);
    std::map<std::tuple<int, int, int>, std::pair<float, MaterialId>> truth;
    {
        DeltaStore::WriteBatch wb(s);
        // A dug tunnel: dense, smooth values (what real edits look like)
        for (int z = -20; z < 20; ++z)
            for (int y = -6; y < 6; ++y)
                for (int x = 0; x < 200; ++x) {
                    const float d = std::sqrt(float(y * y + z * z)) - 4.f;
                    if (std::abs(d) > 3.f) continue;
                    const Vec3i v{x, y, z};
                    const float stored = wb.write(v, -d, d < 0 ? kMatAir : kMatRock);
                    truth[{x, y, z}] = {stored, d < 0 ? MaterialId(kMatAir) : MaterialId(kMatRock)};
                }
    }
    const auto bytes = s.serialize();
    DeltaStore r;
    CHECK(r.deserialize(bytes));
    CHECK(r.overrideCount() == truth.size());
    int mismatch = 0;
    for (auto& [k, val] : truth) {
        auto o = r.read({std::get<0>(k), std::get<1>(k), std::get<2>(k)});
        if (!o || o->density != val.first || o->material != val.second) ++mismatch;
    }
    CHECK(mismatch == 0);
    const double bpv = double(bytes.size()) / double(truth.size());
    std::printf("    %zu overrides -> %zu bytes (%.2f B/voxel, raw float+mat = 5 B)\n", truth.size(), bytes.size(), bpv);
    CHECK(bpv < 3.0);
    auto corrupt = bytes; corrupt[corrupt.size() / 2] ^= 0x5A;
    DeltaStore bad;
    CHECK(!bad.deserialize(corrupt));                       // checksum catches corruption
}

TEST(delta_voids_coarsen_instead_of_vanishing) {
    // A coarse sample stands for s^3 LOD0 voxels. Promoting whichever one lands
    // on the lattice made the coarse field a function of where a hole happened
    // to sit rather than of the hole: the same crater shifted by one voxel gave
    // anywhere from 93 to 136 air samples at LOD1, and every LOD reported the
    // void at its full 6 m depth, so a crater smaller than a cell still carved
    // a cell-sized one. Reducing the whole block fixes both.
    constexpr float kRadius = 6.f;
    constexpr float kProcSolid = -1.f;
    const Vec3i base{5168, 80, -240};          // dead centre of its LOD0 chunk

    auto carve = [](DeltaStore& s, const Vec3i& c) {
        const int R = int(kRadius) + 3;
        DeltaStore::WriteBatch wb(s);
        for (int dz = -R; dz <= R; ++dz)
            for (int dy = -R; dy <= R; ++dy)
                for (int dx = -R; dx <= R; ++dx) {
                    const float d = std::sqrt(float(dx * dx + dy * dy + dz * dz)) - kRadius;
                    if (d > 3.f) continue;      // outside the brush influence band
                    wb.write(Vec3i{c.x + dx, c.y + dy, c.z + dz}, -d, d < 0.f ? kMatAir : kMatRock);
                }
    };
    // Air samples and the deepest point of the void, as the coarse field sees them.
    auto stamp = [&](const DeltaStore& s, const ChunkKey& k, int& air, float& peak) {
        auto f = std::make_unique<DensityField>();
        f->density.fill(kProcSolid);
        f->material.fill(kMatRock);
        s.applyTo(k, *f);
        air = 0;
        peak = -1e9f;
        for (size_t i = 0; i < size_t(kFieldVolume); ++i) {
            if (f->density[i] > 0.f) ++air;
            peak = std::max(peak, f->density[i]);
        }
    };
    auto keyFor = [](const Vec3i& v, int lod) {
        ChunkKey k = DeltaStore::ownerChunk(v);
        for (int i = 0; i < lod; ++i) k = k.parent();
        return k;
    };

    // LOD0 is exact: every air voxel of the sphere lands on its own sample.
    {
        DeltaStore s;
        carve(s, base);
        int air = 0; float peak = 0.f;
        stamp(s, keyFor(base, 0), air, peak);
        const int volume = int(4.0 / 3.0 * 3.14159265 * double(kRadius) * kRadius * kRadius);
        CHECK(air > volume * 9 / 10 && air < volume * 11 / 10);   // ~905 for r=6
        CHECK(std::abs(peak - kRadius) < 0.05f);                  // full depth at the centre
    }

    // Shifting the crater by one voxel changes its lattice alignment and
    // nothing else, so the coarse field must not notice.
    int firstAir = -1;
    for (int o = 0; o < 8; ++o) {
        const Vec3i c{base.x + (o & 1), base.y + ((o >> 1) & 1), base.z + ((o >> 2) & 1)};
        DeltaStore s;
        carve(s, c);
        int air = 0; float peak = 0.f;
        stamp(s, keyFor(c, 1), air, peak);
        if (o == 0) firstAir = air;
        CHECK(air == firstAir);
    }

    // The void has to get shallower as the samples spread out, rather than
    // being reported at full depth from one lucky voxel.
    DeltaStore s;
    carve(s, base);
    float prev = kRadius + 1.f;
    for (int lod = 1; lod <= 4; ++lod) {
        int air = 0; float peak = 0.f;
        stamp(s, keyFor(base, lod), air, peak);
        CHECK(peak < prev);
        prev = peak;
    }
    std::printf("    r=%.0f m crater: LOD1 air %d samples, identical across 8 lattice offsets\n", double(kRadius),
                firstAir);
}

TEST(streaming_terraform_and_reload_are_consistent) {
    TaskSystem ts(3);
    DensityGenerator gen;
    DeltaStore deltas;
    StreamerConfig cfg;
    cfg.maxLod = 3;
    cfg.viewDistance = 600.0;
    ChunkStreamer streamer(ts, gen, deltas, cfg);
    const Vec3d focus{100.0, double(gen.sampleHeight(100.f, 100.f)), 100.0};
    pumpUntilIdle(streamer, focus);
    CHECK(streamer.isIdle());
    CHECK(streamer.stats().readyChunks > 0);
    CHECK(streamer.stats().residentFields > 0);

    Terraformer tf(streamer, gen, deltas);
    VoxelTerrainQuery q(streamer, deltas, gen);
    const Vec3d hole = focus + Vec3d(0, -1.5, 0);
    CHECK(q.density(hole) < 0.f);                           // solid before
    TerraformOp op;
    op.center = hole;
    op.halfExtents = Vec3f(3.f);
    const auto res = tf.apply(op);
    CHECK(res.voxelsWritten > 50);
    CHECK(q.density(hole) > 0.f);                           // air immediately (resident field patched)
    pumpUntilIdle(streamer, focus);
    const ChunkKey owner = ChunkKey::containing(hole, 0);
    const auto meshAfter = streamer.meshFor(owner);
    CHECK(meshAfter && !meshAfter->empty());

    // Reload: a fresh streamer on deserialised deltas must rebuild the same mesh.
    DeltaStore reloaded;
    CHECK(reloaded.deserialize(deltas.serialize()));
    ChunkStreamer s2(ts, gen, reloaded, cfg);
    pumpUntilIdle(s2, focus);
    const auto meshReload = s2.meshFor(owner);
    CHECK(meshReload && meshAfter && meshReload->indices.size() == meshAfter->indices.size());
    VoxelTerrainQuery q2(s2, reloaded, gen);
    CHECK(q2.density(hole) > 0.f);
}

TEST(structure_graph_support_propagation) {
    StructureGraph g;
    // Wood column: grounded base + 9 stacked pieces. 100 - 12.5 * k
    for (PieceId i = 1; i <= 10; ++i) g.addNode(i, StructuralMaterial::Wood, i == 1);
    for (PieceId i = 1; i < 10; ++i) g.connect(i, i + 1, ContactDir::Vertical);
    const auto broken = g.solveAll();
    CHECK(std::abs(g.node(1)->support - 100.f) < 1e-3f);
    CHECK(std::abs(g.node(4)->support - 62.5f) < 1e-3f);
    CHECK(!g.node(8)->broken && g.node(8)->support > 10.f);   // 12.5
    CHECK(g.node(9)->broken && g.node(10)->broken);            // 0 -> broken, and does not transmit
    CHECK(broken.size() == 2);

    // Stone cannot cantilever: horizontal loss = 100%.
    StructureGraph s;
    s.addNode(1, StructuralMaterial::Stone, true);
    s.addNode(2, StructuralMaterial::Stone, false);
    s.connect(1, 2, ContactDir::Horizontal);
    s.solveAll();
    CHECK(s.node(2)->broken);
    // Iron beam can.
    StructureGraph ir;
    ir.addNode(1, StructuralMaterial::Iron, true);
    for (PieceId i = 2; i <= 6; ++i) { ir.addNode(i, StructuralMaterial::Iron, false); ir.connect(i - 1, i, ContactDir::Horizontal); }
    ir.solveAll();
    CHECK(!ir.node(6)->broken);
}

namespace {
struct FlatTerrain : ITerrainQuery {
    double groundY = 0.0;
    double holeMinX = 1e9, holeMaxX = -1e9;     // dug trench
    float density(const Vec3d& p) const override {
        if (p.x > holeMinX && p.x < holeMaxX && p.y > groundY - 3.0) return 1.f;
        return float(p.y - groundY);
    }
};
} // namespace

TEST(building_anchors_and_collapse_on_terrain_edit) {
    FlatTerrain terrain;
    BuildingSystem bs(terrain);
    const auto foundation = bs.registerType({"wood_floor_2x2", {4, 1, 4}, StructuralMaterial::Wood, true});
    const auto wall = bs.registerType({"wood_wall_2x2", {4, 4, 1}, StructuralMaterial::Wood, false});
    const auto beam = bs.registerType({"wood_beam_2m", {4, 1, 1}, StructuralMaterial::Wood, false});

    auto f = bs.tryPlace(foundation, {0.0, 0.0, 0.0});
    CHECK(f.ok());
    CHECK(bs.piece(f.id)->grounded);
    CHECK(bs.tryPlace(foundation, {0.0, 0.0, 0.0}).error == PlacementError::Overlap);
    CHECK(bs.tryPlace(foundation, {10.0, -0.9, 10.0}).error == PlacementError::Buried);

    auto w = bs.tryPlace(wall, {0.0, 0.5, 0.0});
    CHECK(w.ok());
    CHECK(std::abs(w.support - 87.5f) < 1e-3f);                   // one vertical transfer
    // Cantilever beams out from the top of the wall (+x): 87.5-12.5=75 (vertical) then -20 per sideways step.
    auto b1 = bs.tryPlace(beam, {0.0, 2.5, 0.0});
    auto b2 = bs.tryPlace(beam, {2.0, 2.5, 0.0});
    auto b3 = bs.tryPlace(beam, {4.0, 2.5, 0.0});
    auto b4 = bs.tryPlace(beam, {6.0, 2.5, 0.0});
    auto b5 = bs.tryPlace(beam, {8.0, 2.5, 0.0});
    CHECK(b1.ok() && b2.ok() && b3.ok() && b4.ok());               // 75 -> 55 -> 35 -> 15 (>= min 10)
    CHECK(std::abs(b4.support - 15.f) < 1e-3f);
    CHECK(b5.error == PlacementError::Unsupported);                // 15 - 20 < 10
    std::printf("    supports: wall %.1f beams %.1f %.1f %.1f %.1f (5th: %s)\n", w.support, b1.support, b2.support,
                b3.support, b4.support, b5.ok() ? "ok" : "rejected");
    CHECK(bs.pieceCount() == 6);

    // Dig a trench under the foundation: everything loses its anchor and collapses.
    terrain.holeMinX = -1.0; terrain.holeMaxX = 3.0;
    bs.onTerrainEdited({{-1, -3, -1}, {3, 1, 3}});
    const auto collapses = bs.drainCollapses();
    CHECK(collapses.size() == 6);
    CHECK(bs.pieceCount() == 0);
}

TEST(offset_allocator_and_upload_ring) {
    OffsetAllocator a(1000);
    const auto x = a.allocate(100), y = a.allocate(200, 64), z = a.allocate(300);
    CHECK(x != OffsetAllocator::kInvalid && y % 64 == 0 && z != OffsetAllocator::kInvalid);
    a.free(y, 200);
    a.free(x, 100);
    a.free(z, 300);
    CHECK(a.usedBytes() == 0 && a.freeBlockCount() == 1 && a.largestFreeBlock() == 1000);   // fully coalesced

    UploadRing ring(1024);
    CHECK(ring.allocate(600, 1) == 0);
    CHECK(ring.allocate(600, 2) == UploadRing::kInvalid);          // would overlap frame 1
    ring.reclaim(1);
    const auto o = ring.allocate(600, 2);
    CHECK(o != UploadRing::kInvalid);
    CHECK(ring.allocate(500, 3) == UploadRing::kInvalid);          // [0,600) free? no: tail=600? only after reclaim 2
    ring.reclaim(2);
    CHECK(ring.liveBytes() == 0);
}

TEST(material_system_crossfades_when_maps_resident) {
    struct FakeStreamer : ITextureStreamer {
        int framesUntilReady = 3;
        uint64_t request(const std::string&, TextureKind) override { return 1; }
        Poll poll(uint64_t) override { return framesUntilReady-- > 0 ? Poll{Status::Pending, 0} : Poll{Status::Resident, 7}; }
    } streamer;
    MaterialSystem ms(&streamer);
    ms.registerDefaultTerrainMaterials();
    ImageMaps maps; maps.albedo = "rock_albedo.bc7"; maps.normal = "rock_normal.bc5";
    ms.setImages(kMatRock, maps);
    for (int i = 0; i < 3; ++i) { ms.update(1.f / 60.f); CHECK(ms.imageBlend(kMatRock) == 0.f); }
    ms.update(1.f / 60.f);                                          // resident -> slices published, blend starts at 0
    CHECK(ms.gpuTable()[kMatRock].albedoSlice == 7);
    float prev = 0.f;
    bool monotonic = true;
    for (int i = 0; i < 60; ++i) { ms.update(1.f / 60.f); monotonic &= ms.imageBlend(kMatRock) >= prev; prev = ms.imageBlend(kMatRock); }
    CHECK(monotonic && prev == 1.f);
    CHECK(ms.imageBlend(kMatGrass) == 0.f);                         // untouched materials stay procedural
}

#if VXE_WITH_JOLT
TEST(jolt_collision_from_baked_terrain) {
    TaskSystem ts(3);
    DensityGenerator gen;
    DeltaStore deltas;
    StreamerConfig cfg;
    cfg.maxLod = 2;
    cfg.viewDistance = 200.0;
    ChunkStreamer streamer(ts, gen, deltas, cfg);
    PhysicsWorld physics(ts);
    TerrainCollisionBaker baker(ts, physics);
    streamer.addListener(&baker);
    const double groundY = gen.sampleHeight(40.f, 40.f);
    const Vec3d focus{40.0, groundY, 40.0};
    pumpUntilIdle(streamer, focus);
    baker.flush();
    physics.step();                                                // applies baked shapes
    CHECK(baker.stats().liveBodies > 0);

    const auto hit = physics.castRay({40.0, groundY + 20.0, 40.0}, {0.f, -1.f, 0.f}, 60.f);
    CHECK(hit.has_value());
    if (hit) CHECK(std::abs(hit->point.y - groundY) < 1.5);

    const BodyHandle ball = physics.createDynamicSphere({40.0, groundY + 5.0, 40.0}, 0.5f);
    for (int i = 0; i < 240; ++i) physics.step();
    const Vec3d p = physics.bodyPosition(ball);
    std::printf("    ball rest y=%.2f ground=%.2f\n", p.y, groundY);
    CHECK(p.y > groundY - 1.0 && p.y < groundY + 3.0);
}
#endif

#if VXE_WITH_OZZ
TEST(ozz_locomotion_and_foot_ik) {
    FlatTerrain terrain;
    terrain.groundY = 0.0;
    auto rig = CharacterAnimator::makeProceduralBiped();
    CharacterAnimator anim(rig);
    CHECK(anim.jointCount() == 8);
    for (int i = 0; i < 30; ++i) anim.update(1.f / 60.f, 2.5f, {0, 0.0, 0}, 0.3f, &terrain);
    CHECK(anim.phase() > 0.f);
    std::vector<Mat4> palette(size_t(anim.jointCount()));
    anim.skinningPalette(palette);
    CHECK(std::isfinite(palette[3][12]));
    // Raise the ground under the character by 15 cm: IK must keep both feet on it.
    terrain.groundY = 0.15;
    for (int i = 0; i < 30; ++i) anim.update(1.f / 60.f, 0.f, {0, 0.0, 0}, 0.f, &terrain);
    CHECK(anim.leftFootReached() && anim.rightFootReached());
}
#endif

int main() {
    for (auto& t : registry()) {
        const int before = g_failures;
        std::printf("[ RUN  ] %s\n", t.name);
        t.fn();
        std::printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", t.name);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
