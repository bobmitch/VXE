#include "vxe/building/BuildingSystem.h"

#include "vxe/terraform/DeltaStore.h"
#include "vxe/world/ChunkStreamer.h"
#include "vxe/world/DensityGenerator.h"

#include <algorithm>
#include <cmath>

namespace vxe {

// ============================================================================
// Terrain queries
// ============================================================================
std::optional<float> ITerrainQuery::raycast(const Vec3d& from, const Vec3f& dirIn, float maxDistance) const {
    const Vec3d dir(normalize(dirIn));
    constexpr float kStep = 0.25f;
    float prevT = 0.f;
    if (density(from) < 0.f) return 0.f;
    for (float t = kStep; t <= maxDistance + 1e-4f; t += kStep) {
        if (density(from + dir * double(t)) < 0.f) {
            float lo = prevT, hi = t;                   // bisection refine
            for (int i = 0; i < 8; ++i) {
                const float mid = 0.5f * (lo + hi);
                (density(from + dir * double(mid)) < 0.f ? hi : lo) = mid;
            }
            return hi;
        }
        prevT = t;
    }
    return std::nullopt;
}

std::optional<float> ITerrainQuery::raycastDown(const Vec3d& from, float maxDistance) const {
    return raycast(from, Vec3f(0.f, -1.f, 0.f), maxDistance);
}

float VoxelTerrainQuery::latticeDensity(const Vec3i& v) const {
    if (auto r = m_streamer.residentDensity(v)) return *r;
    if (auto o = m_deltas.read(v)) return o->density;
    return m_gen.sampleDensity(v);
}

float VoxelTerrainQuery::density(const Vec3d& p) const {
    const Vec3d g = p / double(world::kVoxelSize);
    const Vec3i v0 = floorToInt(g);
    const Vec3f f(g - Vec3d(v0));
    float c[8];
    for (int i = 0; i < 8; ++i) c[i] = latticeDensity(v0 + Vec3i(i & 1, (i >> 1) & 1, (i >> 2) & 1));
    const float x00 = lerp(c[0], c[1], f.x), x10 = lerp(c[2], c[3], f.x);
    const float x01 = lerp(c[4], c[5], f.x), x11 = lerp(c[6], c[7], f.x);
    return lerp(lerp(x00, x10, f.y), lerp(x01, x11, f.y), f.z);
}

// ============================================================================
// BuildingSystem
// ============================================================================
BuildingSystem::BuildingSystem(const ITerrainQuery& terrain, const BuildingConfig& cfg) : m_terrain(terrain), m_cfg(cfg) {}

PieceTypeId BuildingSystem::registerType(const PieceType& t) {
    m_types.push_back(t);
    return PieceTypeId(m_types.size() - 1);
}

Vec3i BuildingSystem::snap(const Vec3d& p) const { return floorToInt(p / m_cfg.cellSize); }

Vec3i BuildingSystem::rotatedSize(PieceTypeId t, uint8_t rot) const {
    const Vec3i s = m_types[t].sizeCells;
    return (rot & 1) ? Vec3i{s.z, s.y, s.x} : s;
}

namespace {
Aabbd cellBounds(const BuildPiece& p, double cell) { return p.bounds(cell); }
} // namespace

bool BuildingSystem::evaluateGrounded(const BuildPiece& p) const {
    if (!m_types[p.type].canAnchor) return false;
    const Aabbd b = cellBounds(p, m_cfg.cellSize);
    // Sample the base footprint on a 0.25 m lattice, slightly inset.
    const double step = 0.25, inset = 0.05;
    int total = 0, touching = 0;
    for (double x = b.min.x + inset; x <= b.max.x - inset + 1e-9; x += step)
        for (double z = b.min.z + inset; z <= b.max.z - inset + 1e-9; z += step) {
            ++total;
            const Vec3d probe{x, b.min.y - m_cfg.anchorTolerance, z};
            if (m_terrain.density(probe) < 0.f) ++touching;
        }
    return total > 0 && float(touching) >= m_cfg.anchorSampleFraction * float(total);
}

float BuildingSystem::buriedFraction(const BuildPiece& p) const {
    const Aabbd b = cellBounds(p, m_cfg.cellSize);
    int total = 0, solid = 0;
    for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i) {
                const Vec3d t{(i + 0.5) / 3.0, (j + 0.5) / 3.0, (k + 0.5) / 3.0};
                const Vec3d q = b.min + (b.max - b.min) * t;
                ++total;
                if (m_terrain.density(q) < 0.f) ++solid;
            }
    return float(solid) / float(total);
}

std::vector<std::pair<PieceId, ContactDir>> BuildingSystem::findContacts(const BuildPiece& p) const {
    std::vector<std::pair<PieceId, ContactDir>> out;
    auto probe = [&](const Vec3i& c, ContactDir dir) {
        auto it = m_cells.find(c);
        if (it == m_cells.end() || it->second == p.id) return;
        for (auto& e : out)
            if (e.first == it->second) { if (dir == ContactDir::Vertical) e.second = dir; return; }
        out.emplace_back(it->second, dir);
    };
    const Vec3i a = p.cellMin, b = p.cellMax;
    for (int32_t x = a.x; x <= b.x; ++x)
        for (int32_t z = a.z; z <= b.z; ++z) {
            probe({x, a.y - 1, z}, ContactDir::Vertical);
            probe({x, b.y + 1, z}, ContactDir::Vertical);
        }
    for (int32_t y = a.y; y <= b.y; ++y) {
        for (int32_t z = a.z; z <= b.z; ++z) {
            probe({a.x - 1, y, z}, ContactDir::Horizontal);
            probe({b.x + 1, y, z}, ContactDir::Horizontal);
        }
        for (int32_t x = a.x; x <= b.x; ++x) {
            probe({x, y, a.z - 1}, ContactDir::Horizontal);
            probe({x, y, b.z + 1}, ContactDir::Horizontal);
        }
    }
    return out;
}

void BuildingSystem::occupy(const BuildPiece& p, bool add) {
    for (int32_t z = p.cellMin.z; z <= p.cellMax.z; ++z)
        for (int32_t y = p.cellMin.y; y <= p.cellMax.y; ++y)
            for (int32_t x = p.cellMin.x; x <= p.cellMax.x; ++x) {
                if (add) m_cells[{x, y, z}] = p.id;
                else m_cells.erase({x, y, z});
            }
}

PlacementResult BuildingSystem::tryPlace(PieceTypeId typeId, const Vec3d& worldPos, uint8_t rotation) {
    PlacementResult r;
    if (typeId >= m_types.size()) { r.error = PlacementError::UnknownType; return r; }

    BuildPiece p;
    p.id = m_nextId;
    p.type = typeId;
    p.rotation = rotation & 3;
    p.cellMin = snap(worldPos);
    p.cellMax = p.cellMin + rotatedSize(typeId, p.rotation) - Vec3i(1);

    for (int32_t z = p.cellMin.z; z <= p.cellMax.z; ++z)
        for (int32_t y = p.cellMin.y; y <= p.cellMax.y; ++y)
            for (int32_t x = p.cellMin.x; x <= p.cellMax.x; ++x)
                if (m_cells.count({x, y, z})) { r.error = PlacementError::Overlap; return r; }
    if (buriedFraction(p) > m_cfg.maxBuriedFraction) { r.error = PlacementError::Buried; return r; }

    p.grounded = evaluateGrounded(p);
    const auto contacts = findContacts(p);
    m_graph.addNode(p.id, m_types[typeId].material, p.grounded);
    for (const auto& [other, dir] : contacts) m_graph.connect(p.id, other, dir);
    const PieceId seed[] = {p.id};
    m_graph.solve(seed);

    const StructureGraph::Node* n = m_graph.node(p.id);
    if (n->broken) {
        m_graph.removeNode(p.id);
        std::vector<PieceId> neighbours;
        for (const auto& [o, d] : contacts) neighbours.push_back(o);
        m_graph.solve(neighbours);   // restore neighbours' values (unchanged; broken pieces transmit nothing)
        r.error = PlacementError::Unsupported;
        return r;
    }
    ++m_nextId;
    occupy(p, true);
    m_pieces.emplace(p.id, p);
    r.id = p.id;
    r.support = n->support;
    return r;
}

void BuildingSystem::collapse(std::vector<PieceId> broken) {
    for (PieceId id : broken) {
        auto it = m_pieces.find(id);
        if (it == m_pieces.end()) continue;
        m_collapses.push_back({id, it->second.type, cellBounds(it->second, m_cfg.cellSize), m_types[it->second.type].material});
        occupy(it->second, false);
        m_graph.removeNode(id);
        m_pieces.erase(it);
    }
}

void BuildingSystem::remove(PieceId id) {
    const StructureGraph::Node* n = m_graph.node(id);
    if (!n) return;
    std::vector<PieceId> neighbours;
    for (const auto& [o, d] : n->edges) neighbours.push_back(o);
    auto it = m_pieces.find(id);
    occupy(it->second, false);
    m_graph.removeNode(id);
    m_pieces.erase(it);
    collapse(m_graph.solve(neighbours));
}

void BuildingSystem::onTerrainEdited(const Aabbd& region) {
    const Aabbd r = region.expanded(1.0);
    std::vector<PieceId> seeds;
    for (auto& [id, p] : m_pieces) {
        if (!cellBounds(p, m_cfg.cellSize).expanded(m_cfg.anchorTolerance + 0.25).intersects(r)) continue;
        const bool g = evaluateGrounded(p);
        if (g != p.grounded) {
            p.grounded = g;
            m_graph.setGrounded(id, g);
            seeds.push_back(id);
        }
    }
    if (!seeds.empty()) collapse(m_graph.solve(seeds));
}

bool BuildingSystem::allowsTerrainEdit(const Aabbd& region) const {
    if (m_cfg.editPolicy == TerrainEditPolicy::AllowAndCollapse) return true;
    for (const auto& [id, p] : m_pieces)
        if (p.grounded && cellBounds(p, m_cfg.cellSize).expanded(0.5).intersects(region)) return false;
    return true;
}

const BuildPiece* BuildingSystem::piece(PieceId id) const {
    auto it = m_pieces.find(id);
    return it == m_pieces.end() ? nullptr : &it->second;
}

float BuildingSystem::support(PieceId id) const {
    const StructureGraph::Node* n = m_graph.node(id);
    return n ? n->support : 0.f;
}

} // namespace vxe
