#include "vxe/meshing/DualContouring.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <vector>

namespace vxe {

namespace {

constexpr int32_t N = kFieldCells;     // 32 owned cells
constexpr int32_t C = N + 1;           // 33 visited cells per axis
constexpr int32_t kNoVertex = -1;

// Corner c = i | j<<1 | k<<2 ; offsets in (x, y, z)
constexpr std::array<std::array<int, 3>, 8> kCorner{{
    {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1},
}};
constexpr std::array<std::array<int, 2>, 12> kEdges{{
    {0, 1}, {2, 3}, {4, 5}, {6, 7},   // x
    {0, 2}, {1, 3}, {4, 6}, {5, 7},   // y
    {0, 4}, {1, 5}, {2, 6}, {3, 7},   // z
}};

// --- Symmetric 3x3 helpers for the QEF --------------------------------------
struct Sym3 {
    float m00 = 0, m01 = 0, m02 = 0, m11 = 0, m12 = 0, m22 = 0;
    Vec3f mul(const Vec3f& v) const {
        return {m00 * v.x + m01 * v.y + m02 * v.z, m01 * v.x + m11 * v.y + m12 * v.z, m02 * v.x + m12 * v.y + m22 * v.z};
    }
};

// Cyclic Jacobi eigen-decomposition: A = V diag(w) V^T.
void jacobiEigen(const Sym3& s, float w[3], float V[3][3]) {
    float a[3][3] = {{s.m00, s.m01, s.m02}, {s.m01, s.m11, s.m12}, {s.m02, s.m12, s.m22}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) V[i][j] = i == j ? 1.f : 0.f;

    for (int sweep = 0; sweep < 8; ++sweep) {
        const float off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        if (off < 1e-12f) break;
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(a[p][q]) < 1e-9f) continue;
                const float theta = (a[q][q] - a[p][p]) / (2.f * a[p][q]);
                const float t = (theta >= 0.f ? 1.f : -1.f) / (std::abs(theta) + std::sqrt(theta * theta + 1.f));
                const float c = 1.f / std::sqrt(t * t + 1.f), sn = t * c;
                for (int k = 0; k < 3; ++k) {          // A = J^T A J
                    const float akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - sn * akq;
                    a[k][q] = sn * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const float apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - sn * aqk;
                    a[q][k] = sn * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {          // V = V J
                    const float vkp = V[k][p], vkq = V[k][q];
                    V[k][p] = c * vkp - sn * vkq;
                    V[k][q] = sn * vkp + c * vkq;
                }
            }
        }
    }
    w[0] = a[0][0]; w[1] = a[1][1]; w[2] = a[2][2];
}

struct Qef {
    Sym3 ata;
    Vec3f atb{};
    float btb = 0.f;
    Vec3f massSum{};
    int count = 0;

    void add(const Vec3f& p, const Vec3f& n) {
        ata.m00 += n.x * n.x; ata.m01 += n.x * n.y; ata.m02 += n.x * n.z;
        ata.m11 += n.y * n.y; ata.m12 += n.y * n.z; ata.m22 += n.z * n.z;
        const float d = dot(n, p);
        atb += n * d;
        btb += d * d;
        massSum += p;
        ++count;
    }

    // Returns position (cell-local, [0,1]^3), rank, residual.
    Vec3f solve(float relThreshold, int& rank, float& error) const {
        const Vec3f mass = massSum / float(std::max(count, 1));
        const Vec3f rhs = atb - ata.mul(mass);
        float w[3], V[3][3];
        jacobiEigen(ata, w, V);
        const float wmax = std::max({std::abs(w[0]), std::abs(w[1]), std::abs(w[2])});
        rank = 0;
        Vec3f x{};
        if (wmax > 1e-6f) {
            for (int i = 0; i < 3; ++i) {
                if (std::abs(w[i]) < relThreshold * wmax) continue;
                ++rank;
                const Vec3f vi{V[0][i], V[1][i], V[2][i]};
                x += vi * (dot(vi, rhs) / w[i]);   // pinv = sum v v^T / w
            }
        }
        const Vec3f pos = mass + x;
        error = std::max(0.f, dot(pos, ata.mul(pos)) - 2.f * dot(pos, atb) + btb);
        return pos;
    }
};

// Open-addressed table of mesh edges keyed by the unordered vertex pair, used
// to find the ones a single triangle touches. Linear probing over a power-of-
// two table; it lives in the thread-local scratch so skirting allocates nothing
// per chunk, and its layout is a pure function of the insertion order, so the
// skirt comes out identical on every machine.
struct EdgeTable {
    static constexpr uint64_t kEmpty = ~uint64_t(0);
    struct Slot {
        uint64_t key = kEmpty;
        uint32_t count = 0;
        uint32_t first = 0;   // the 'a' of the first directed edge (a, b) seen
    };
    std::vector<Slot> slots;
    uint32_t mask = 0;

    void reset(size_t edgeCount) {
        size_t cap = 64;
        while (cap < edgeCount * 2) cap <<= 1;             // keep the load factor under 1/2
        if (slots.size() == cap) std::fill(slots.begin(), slots.end(), Slot{});
        else slots.assign(cap, Slot{});
        mask = uint32_t(cap - 1);
    }
    void add(uint32_t a, uint32_t b) {
        const uint64_t key = a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a;
        for (uint32_t i = uint32_t(mix64(key)) & mask;; i = (i + 1) & mask) {
            if (slots[i].key == kEmpty) { slots[i] = {key, 1, a}; return; }
            if (slots[i].key == key) { ++slots[i].count; return; }
        }
    }
};

struct Scratch {
    std::vector<int32_t> cellVertex = std::vector<int32_t>(size_t(C * C * C), kNoVertex);
    std::vector<Vec3f> gradient = std::vector<Vec3f>(size_t(kFieldVolume));
    EdgeTable edges;
    std::vector<int32_t> skirtVertex;
};
Scratch& threadScratch() { thread_local Scratch s; return s; }

constexpr int32_t cellIndex(int32_t x, int32_t y, int32_t z) { return x + C * (y + C * z); }

// Trilinear density at a chunk-local *grid point* coordinate, clamped to the
// sampled range so the stencil never runs off the padded field.
float sampleFieldTrilinear(const DensityField& f, Vec3f p) {
    constexpr float lo = float(-kFieldOffset);
    constexpr float hi = float(kFieldSize - kFieldOffset - 2);   // leaves room for the +1 corner
    p = vclamp(p, Vec3f(lo), Vec3f(hi));
    const int32_t x0 = int32_t(std::floor(p.x)), y0 = int32_t(std::floor(p.y)), z0 = int32_t(std::floor(p.z));
    const float fx = p.x - float(x0), fy = p.y - float(y0), fz = p.z - float(z0);
    const float c00 = lerp(f.at(x0, y0, z0), f.at(x0 + 1, y0, z0), fx);
    const float c10 = lerp(f.at(x0, y0 + 1, z0), f.at(x0 + 1, y0 + 1, z0), fx);
    const float c01 = lerp(f.at(x0, y0, z0 + 1), f.at(x0 + 1, y0, z0 + 1), fx);
    const float c11 = lerp(f.at(x0, y0 + 1, z0 + 1), f.at(x0 + 1, y0 + 1, z0 + 1), fx);
    return lerp(lerp(c00, c10, fy), lerp(c01, c11, fy), fz);
}

// How far back along -n the collar can go from `localPos` before it leaves the
// solid. Without this clamp a collar punches straight through anything thinner
// than its depth - a cave roof is often two metres of rock - and the part that
// comes out the far side hangs in the air above the terrain, lit by the normal
// of the ceiling it came from, i.e. a dark wing.
float solidReach(const DensityField& f, const Vec3f& localPos, const Vec3f& n, float maxDepth, float voxelStep) {
    const int steps = std::clamp(int(std::ceil(maxDepth / voxelStep * 2.f)), 4, 24);
    const float inv = 1.f / voxelStep;
    float reached = 0.f;
    for (int i = 1; i <= steps; ++i) {
        const float t = maxDepth * float(i) / float(steps);
        if (sampleFieldTrilinear(f, (localPos - n * t) * inv) >= 0.f) break;   // back out in the air
        reached = t;
    }
    return reached;
}

// A chunk is meshed in isolation, so the only open boundary its mesh has is the
// chunk perimeter: every interior edge is shared by two triangles, including
// the quad diagonals. Hanging a collar off each open edge along -normal gives
// geometry that sits under the neighbour's surface wherever the two agree, and
// plugs the gap wherever they do not (an inter-LOD seam). Each collar vertex
// goes as deep as the solid allows, up to `depth`, so the collar tapers away
// rather than bursting out of thin terrain.
//
// Appends to `m` and returns the triangle count. `m.vertices` grows, so the
// caller must refresh the bounds afterwards.
uint32_t appendBorderSkirt(ChunkMesh& m, float depth, float voxelStep, const DensityField& field, Scratch& sc,
                           uint32_t& openEdges) {
    if (depth <= 0.f || m.indices.empty()) return 0;

    sc.edges.reset(m.indices.size());
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
        for (int k = 0; k < 3; ++k)
            sc.edges.add(m.indices[t + size_t(k)], m.indices[t + size_t((k + 1) % 3)]);

    // One skirt vertex per perimeter vertex, not per edge, so the collar is a
    // connected strip rather than a fan of detached quads.
    sc.skirtVertex.assign(m.vertices.size(), kNoVertex);
    auto skirtOf = [&](uint32_t v) {
        int32_t& s = sc.skirtVertex[v];
        if (s == kNoVertex) {
            TerrainVertex sv = m.vertices[v];          // copy: the push_back below may reallocate
            sv.position -= sv.normal * solidReach(field, sv.position, sv.normal, depth, voxelStep);
            s = int32_t(m.vertices.size());
            m.vertices.push_back(sv);
        }
        return uint32_t(s);
    };

    uint32_t triangles = 0;
    for (const EdgeTable::Slot& slot : sc.edges.slots) {
        if (slot.key == EdgeTable::kEmpty || slot.count != 1) continue;
        ++openEdges;
        const uint32_t lo = uint32_t(slot.key >> 32), hi = uint32_t(slot.key & 0xFFFFFFFFu);
        const uint32_t a = slot.first, b = slot.first == lo ? hi : lo;
        const uint32_t a2 = skirtOf(a), b2 = skirtOf(b);
        // Wound so the collar faces away from the triangle it hangs off, i.e.
        // towards whoever is looking into the crack.
        m.indices.insert(m.indices.end(), {a, a2, b2, a, b2, b});
        triangles += 2;
    }
    return triangles;
}

} // namespace

MesherStats contourChunk(const DensityField& field, float voxelStep, const MesherSettings& settings, ChunkMesh& out) {
    out.clear();
    MesherStats stats;
    Scratch& sc = threadScratch();
    std::fill(sc.cellVertex.begin(), sc.cellVertex.end(), kNoVertex);

    const auto& d = field.density;

    // ---- 1. Gradients at all points the mesher can touch (points [0, N+1]) ----
    // ENO (essentially non-oscillatory) differences: where the field has a
    // crease (carved edges, max/min CSG of edits, ridged noise) a central
    // difference straddles both sides and smears the normal, which is exactly
    // what destroys DC's sharp features. ENO picks the one-sided stencil whose
    // neighbouring second difference is smaller, i.e. the side that does not
    // cross the crease; in smooth regions it reduces to a central difference.
    // Scale (1/step) is irrelevant because normals are normalised.
    auto enoDiff = [&](int32_t x, int32_t y, int32_t z, int axis) -> float {
        const int32_t sx = axis == 0, sy = axis == 1, sz = axis == 2;
        auto at = [&](int32_t k) { return d[size_t(DensityField::index(x + sx * k, y + sy * k, z + sz * k))]; };
        const int32_t coord = axis == 0 ? x : (axis == 1 ? y : z);
        const float dm = at(0) - at(-1), dp = at(1) - at(0);
        const float central = 0.5f * (dm + dp);
        if (coord < 2 || coord > kFieldSize - 3) return central;
        const float c2 = std::abs(dp - dm);
        if (c2 < 1e-3f) return central;                       // locally linear: central is exact
        const float cm = std::abs(at(0) - 2.f * at(-1) + at(-2));
        const float cp = std::abs(at(2) - 2.f * at(1) + at(0));
        if (std::abs(cm - cp) < 1e-3f) return central;
        return cm < cp ? dm : dp;
    };
    for (int32_t z = 1; z < kFieldSize - 1; ++z)
        for (int32_t y = 1; y < kFieldSize - 1; ++y)
            for (int32_t x = 1; x < kFieldSize - 1; ++x)
                sc.gradient[size_t(DensityField::index(x, y, z))] = {enoDiff(x, y, z, 0), enoDiff(x, y, z, 1),
                                                                     enoDiff(x, y, z, 2)};

    Vec3f bmin(1e30f), bmax(-1e30f);

    // ---- 2. One vertex per sign-changing cell (cells [0, N]) ----------------
    for (int32_t cz = 0; cz < C; ++cz)
        for (int32_t cy = 0; cy < C; ++cy)
            for (int32_t cx = 0; cx < C; ++cx) {
                float cd[8];
                int32_t ci[8];
                uint32_t mask = 0;
                for (int c = 0; c < 8; ++c) {
                    ci[c] = DensityField::pointIndex(cx + kCorner[c][0], cy + kCorner[c][1], cz + kCorner[c][2]);
                    cd[c] = d[size_t(ci[c])];
                    if (cd[c] < 0.f) mask |= 1u << c;
                }
                if (mask == 0 || mask == 0xFF) continue;
                ++stats.activeCells;

                Qef qef;
                Vec3f normalSum{};
                for (const auto& e : kEdges) {
                    const int a = e[0], b = e[1];
                    if (((mask >> a) & 1) == ((mask >> b) & 1)) continue;
                    const float t = std::clamp(cd[a] / (cd[a] - cd[b]), 0.f, 1.f);
                    const Vec3f pa{float(kCorner[a][0]), float(kCorner[a][1]), float(kCorner[a][2])};
                    const Vec3f pb{float(kCorner[b][0]), float(kCorner[b][1]), float(kCorner[b][2])};
                    const Vec3f p = pa + (pb - pa) * t;                       // cell-local [0,1]^3
                    Vec3f g = sc.gradient[size_t(ci[a])] * (1.f - t) + sc.gradient[size_t(ci[b])] * t;
                    if (dot(g, g) < 1e-12f) g = (pb - pa) * (cd[b] > cd[a] ? 1.f : -1.f);
                    const Vec3f n = normalize(g);
                    qef.add(p, n);
                    normalSum += n;
                }

                int rank = 0;
                float err = 0.f;
                const Vec3f mass = qef.massSum / float(qef.count);
                Vec3f v = qef.solve(settings.svdRelativeThreshold, rank, err);
                // Keep the vertex inside its cell: guarantees a manifold-ish result and
                // makes the boundary cell identical in both neighbouring chunks.
                const bool outside = v.x < 0.f || v.y < 0.f || v.z < 0.f || v.x > 1.f || v.y > 1.f || v.z > 1.f;
                if (outside || err > settings.maxQefErrorCells) v = vclamp(v, Vec3f(0.f), Vec3f(1.f));
                if (err > settings.maxQefErrorCells * 4.f) v = mass;
                v = mass + (v - mass) * settings.sharpness;
                if (rank >= 2) ++stats.sharpVertices;

                // Materials: dominant + secondary among solid corners.
                int counts[kMatCount] = {};
                for (int c = 0; c < 8; ++c)
                    if (mask & (1u << c)) {
                        const MaterialId m = field.material[size_t(ci[c])];
                        counts[m < kMatCount ? int(m) : int(kMatRock)]++;
                    }
                int m0 = kMatRock, m1 = kMatRock;
                for (int m = 1; m < kMatCount; ++m) if (counts[m] > counts[m0]) m0 = m;
                int best1 = -1;
                for (int m = 1; m < kMatCount; ++m) if (m != m0 && counts[m] > best1) { best1 = counts[m]; m1 = m; }
                const int solid = std::popcount(mask);
                const float blend = best1 > 0 ? float(best1) / float(solid) : 0.f;
                const float cavity = float(solid) / 8.f; // more solid corners -> more occluded

                TerrainVertex vert;
                vert.position = (Vec3f(float(cx), float(cy), float(cz)) + v) * voxelStep;
                vert.normal = normalize(normalSum);
                vert.material = packMaterial(MaterialId(m0), MaterialId(best1 > 0 ? m1 : m0), blend, cavity);
                sc.cellVertex[size_t(cellIndex(cx, cy, cz))] = int32_t(out.vertices.size());
                out.vertices.push_back(vert);
                bmin = vmin(bmin, vert.position);
                bmax = vmax(bmax, vert.position);
            }

    // ---- 3. One quad per owned sign-changing edge -----------------------------
    // For axis ax, u=(ax+1)%3, v=(ax+2)%3. Cells around edge at base point b:
    //   q0 = b - eu - ev, q1 = b - ev, q2 = b, q3 = b - eu
    // which wind CCW seen from +ax. Ownership: b[ax] in [0,N-1], b[u], b[v] in [1,N].
    for (int ax = 0; ax < 3; ++ax) {
        const int u = (ax + 1) % 3, v = (ax + 2) % 3;
        Vec3i p;
        for (p[2] = (ax == 2 ? 0 : 1); p[2] <= (ax == 2 ? N - 1 : N); ++p[2])
            for (p[1] = (ax == 1 ? 0 : 1); p[1] <= (ax == 1 ? N - 1 : N); ++p[1])
                for (p[0] = (ax == 0 ? 0 : 1); p[0] <= (ax == 0 ? N - 1 : N); ++p[0]) {
                    Vec3i q = p;
                    q[ax] += 1;
                    const float da = field.at(p.x, p.y, p.z);
                    const float db = field.at(q.x, q.y, q.z);
                    const bool sa = da < 0.f, sb = db < 0.f;
                    if (sa == sb) continue;

                    Vec3i c[4] = {p, p, p, p};
                    c[0][u] -= 1; c[0][v] -= 1;
                    c[1][v] -= 1;
                    c[3][u] -= 1;
                    int32_t vi[4];
                    bool ok = true;
                    for (int k = 0; k < 4; ++k) {
                        vi[k] = sc.cellVertex[size_t(cellIndex(c[k].x, c[k].y, c[k].z))];
                        ok &= vi[k] != kNoVertex;
                    }
                    if (!ok) continue;   // cannot happen for consistent input; defensive

                    // Solid at the low end => surface faces +ax => keep CCW order.
                    if (!sa) { std::swap(vi[1], vi[3]); }

                    // Split along the shorter diagonal for better triangle quality.
                    const Vec3f& P0 = out.vertices[size_t(vi[0])].position;
                    const Vec3f& P1 = out.vertices[size_t(vi[1])].position;
                    const Vec3f& P2 = out.vertices[size_t(vi[2])].position;
                    const Vec3f& P3 = out.vertices[size_t(vi[3])].position;
                    const Vec3f d02 = P2 - P0, d13 = P3 - P1;
                    if (dot(d02, d02) <= dot(d13, d13)) {
                        out.indices.insert(out.indices.end(), {uint32_t(vi[0]), uint32_t(vi[1]), uint32_t(vi[2]),
                                                               uint32_t(vi[0]), uint32_t(vi[2]), uint32_t(vi[3])});
                    } else {
                        out.indices.insert(out.indices.end(), {uint32_t(vi[0]), uint32_t(vi[1]), uint32_t(vi[3]),
                                                               uint32_t(vi[1]), uint32_t(vi[2]), uint32_t(vi[3])});
                    }
                }
    }

    // ---- 4. Border skirt ------------------------------------------------------
    out.skirtIndexStart = out.indices.size();
    if (settings.skirtDepthCells > 0.f) {
        const size_t surfaceVertices = out.vertices.size();
        stats.skirtTriangles =
            appendBorderSkirt(out, settings.skirtDepthCells * voxelStep, voxelStep, field, sc, stats.openEdges);
        for (size_t i = surfaceVertices; i < out.vertices.size(); ++i) {
            bmin = vmin(bmin, out.vertices[i].position);
            bmax = vmax(bmax, out.vertices[i].position);
        }
    }

    if (!out.vertices.empty()) out.bounds = {bmin, bmax};
    return stats;
}

} // namespace vxe
