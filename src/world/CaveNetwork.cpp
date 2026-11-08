#include "vxe/world/CaveNetwork.h"

#include "vxe/core/Deterministic.h"

#include <algorithm>
#include <cmath>

namespace vxe {
namespace {

// Distance to a capsule, with the radius interpolated along it.
float capsuleDistance(const Vec3f& p, const CaveSegment& s) {
    const Vec3f ab = s.b - s.a;
    const Vec3f ap = p - s.a;
    const float denom = dot(ab, ab);
    const float t = denom > 1e-6f ? std::clamp(dot(ap, ab) / denom, 0.f, 1.f) : 0.f;
    const Vec3f closest = s.a + ab * t;
    return length(p - closest) - lerp(s.ra, s.rb, t);
}

// Ellipsoids are not a true distance field; scaling the Euclidean distance by
// the smallest radius gives a conservative, smooth approximation, which is all
// the mesher needs to find the surface.
float chamberDistance(const Vec3f& p, const CaveChamber& c) {
    const Vec3f d{(p.x - c.centre.x) / c.radii.x, (p.y - c.centre.y) / c.radii.y, (p.z - c.centre.z) / c.radii.z};
    const float k = length(d);
    const float minR = std::min(c.radii.x, std::min(c.radii.y, c.radii.z));
    return (k - 1.f) * minR;
}

void growBounds(Aabbf& b, const Vec3f& c, float r) {
    b.min = vmin(b.min, c - Vec3f(r));
    b.max = vmax(b.max, c + Vec3f(r));
}

// Where would water collect? Probe the height field across the cell and take
// the candidates that sit lowest relative to their immediate surroundings --
// a basin rather than merely a low place, so an entrance lands in a hollow the
// ground actually drains into rather than halfway down a uniform slope.
struct Candidate {
    Vec3f pos;
    float score;        // higher is more basin-like
    Vec3f downhill;     // surface flow direction there, the trunk's first bearing
};

std::vector<Candidate> findBasins(const CaveParams& p, int32_t seed, float x0, float z0, const HeightFn& height) {
    constexpr float kProbe = 40.f;      // how far out the surroundings are sampled
    std::vector<Candidate> out;
    out.reserve(size_t(p.entranceCandidates));

    det::Stream rs(det::hash2(int32_t(x0), int32_t(z0), seed ^ 0x51ED27));
    float meanH = 0.f;
    for (int32_t i = 0; i < p.entranceCandidates; ++i) {
        // Jittered, but inset so a system stays inside its own cell.
        const float cx = x0 + rs.range(0.18f, 0.82f) * p.regionSize;
        const float cz = z0 + rs.range(0.18f, 0.82f) * p.regionSize;
        const float h = height(cx, cz);
        const float hxp = height(cx + kProbe, cz), hxm = height(cx - kProbe, cz);
        const float hzp = height(cx, cz + kProbe), hzm = height(cx, cz - kProbe);
        // Positive where the surroundings are higher on average: a hollow.
        const float bowl = (hxp + hxm + hzp + hzm) * 0.25f - h;
        // Flat ground scores better too; water does not pool on a hillside.
        const float gx = (hxp - hxm) * (0.5f / kProbe), gz = (hzp - hzm) * (0.5f / kProbe);
        const float slope = std::sqrt(gx * gx + gz * gz);
        Candidate c;
        c.pos = {cx, h, cz};
        c.score = bowl - slope * 18.f;
        // Downhill, for the trunk to set off along. Falls back to a fixed
        // bearing on ground flat enough to have no direction at all.
        c.downhill = slope > 1e-4f ? normalize(Vec3f{-gx, 0.f, -gz}) : Vec3f{1.f, 0.f, 0.f};
        meanH += h;
        out.push_back(c);
    }
    meanH /= float(p.entranceCandidates);

    // Prefer candidates below the cell's mean: the system should hang off a
    // valley, not a shoulder.
    for (Candidate& c : out) c.score += (meanH - c.pos.y) * 0.08f;
    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    return out;
}

// Walks one passage downhill from `from`, laying capsules, and branching. The
// recursion is bounded by depth rather than by segment count so a system cannot
// run away.
void walk(const CaveParams& p, det::Stream& rs, const Vec3f& from, float bearing, float descent, int32_t steps,
          int32_t branchDepth, float entranceY, const Aabbf& allowed, CaveSystem& sys) {
    Vec3f cur = from;
    float yaw = bearing;
    float radius = p.tunnelRadius + rs.range(-p.tunnelRadiusVar, p.tunnelRadiusVar);

    for (int32_t i = 0; i < steps; ++i) {
        // Wander. Scaled by depth so the first few steps stay near the bearing
        // the surface handed us and the passage reads as going *in* somewhere.
        const float settle = std::min(1.f, float(i) * 0.25f);
        yaw += rs.range(-p.turnRate, p.turnRate) * settle;

        const float step = p.stepLength * rs.range(0.75f, 1.25f);
        const float drop = step * descent * rs.range(0.5f, 1.4f);
        Vec3f next{cur.x + det::cos(yaw) * step, cur.y - drop, cur.z + det::sin(yaw) * step};

        // Flatten out at depth rather than boring to bedrock.
        if (next.y < entranceY - p.maxDepth || next.y < p.bedrockY + 20.f) {
            next.y = std::max(cur.y - drop * 0.1f, p.bedrockY + 20.f);
            descent *= 0.25f;
        }

        // A system has to stay inside its own cell's allowance, because a chunk
        // only asks the cells near it which caves might reach it. A passage
        // that wandered further than that would simply stop existing once the
        // chunks around it stopped asking its cell.
        if (next.x < allowed.min.x || next.x > allowed.max.x || next.z < allowed.min.z || next.z > allowed.max.z)
            return;

        const float nextRadius = std::max(1.4f, radius + rs.range(-0.5f, 0.5f));
        CaveSegment seg{cur, next, radius, nextRadius};
        sys.segments.push_back(seg);
        growBounds(sys.bounds, cur, radius);
        growBounds(sys.bounds, next, nextRadius);

        cur = next;
        radius = nextRadius;

        // Chambers where a passage turns a corner or splits: the places a
        // player would naturally stop, which is where a destination belongs.
        const bool branching = branchDepth < p.maxBranchDepth && rs.chance(p.branchChance);
        if (branching || rs.chance(p.chamberChance * 0.25f)) {
            if (rs.chance(p.chamberChance)) {
                CaveChamber ch;
                const float r = p.chamberRadius + rs.range(-p.chamberRadiusVar, p.chamberRadiusVar);
                ch.centre = cur;
                // Wider than tall: a room, not a bubble.
                ch.radii = {r * rs.range(0.9f, 1.5f), r * rs.range(0.45f, 0.8f), r * rs.range(0.9f, 1.5f)};
                sys.chambers.push_back(ch);
                growBounds(sys.bounds, ch.centre, std::max(ch.radii.x, std::max(ch.radii.y, ch.radii.z)));
            }
        }
        if (branching) {
            const float turn = rs.range(0.6f, 1.5f) * (rs.chance(0.5f) ? 1.f : -1.f);
            walk(p, rs, cur, yaw + turn, descent * rs.range(0.5f, 1.2f), std::max(4, steps / 2), branchDepth + 1,
                 entranceY, allowed, sys);
        }
    }
}

} // namespace

CaveSystem buildCaveSystem(const CaveParams& p, int32_t seed, int32_t cellX, int32_t cellZ, const HeightFn& height) {
    CaveSystem sys;
    // Does this cell hold a system at all? Most do not.
    if (det::unit(det::hash2(cellX, cellZ, seed ^ 0x3C9A17)) >= p.regionDensity) return sys;

    const float x0 = float(cellX) * p.regionSize, z0 = float(cellZ) * p.regionSize;
    const std::vector<Candidate> basins = findBasins(p, seed, x0, z0, height);
    if (basins.empty()) return sys;

    // How far outside its own cell a system may stray. Chunk queries widen by
    // the same amount, so the two always agree about who owns what.
    const float slack = p.regionSize * kCellSlack;
    const Aabbf allowed{{x0 - slack, -1e30f, z0 - slack}, {x0 + p.regionSize + slack, 1e30f, z0 + p.regionSize + slack}};

    det::Stream rs(det::hash2(cellX, cellZ, seed ^ 0x7F4A7C15));
    const int32_t wanted = std::min(p.entrancesPerSystem, int32_t(basins.size()));
    for (int32_t e = 0; e < wanted; ++e) {
        const Candidate& c = basins[size_t(e)];
        // Sea floor and below is not where a walk-in entrance belongs.
        if (c.pos.y < 3.f) continue;

        // Start just under the surface and head in along the way the ground
        // drains. The first segment rises to meet the surface so the mouth is
        // open rather than sealed under a metre of rock.
        const Vec3f mouth{c.pos.x, c.pos.y + 1.5f, c.pos.z};
        const float bearing = std::atan2(c.downhill.z, c.downhill.x);
        const Vec3f inside{c.pos.x + c.downhill.x * 8.f, c.pos.y - 6.f, c.pos.z + c.downhill.z * 8.f};

        CaveSegment throat{mouth, inside, p.tunnelRadius * 0.85f, p.tunnelRadius};
        sys.segments.push_back(throat);
        sys.entrances.push_back(mouth);
        growBounds(sys.bounds, mouth, p.tunnelRadius);
        growBounds(sys.bounds, inside, p.tunnelRadius);

        walk(p, rs, inside, bearing, p.descentRate, p.trunkSteps, 0, c.pos.y, allowed, sys);
    }
    return sys;
}

void splatCaves(const CaveSystem& sys, const Vec3f& origin, float step, int32_t n, float grow, float clampDist,
                float* out) {
    const float invStep = 1.f / step;
    const auto rasterise = [&](const Vec3f& lo, const Vec3f& hi, auto&& distance) {
        const int32_t x0 = std::max(0, int32_t(std::floor((lo.x - origin.x) * invStep)));
        const int32_t y0 = std::max(0, int32_t(std::floor((lo.y - origin.y) * invStep)));
        const int32_t z0 = std::max(0, int32_t(std::floor((lo.z - origin.z) * invStep)));
        const int32_t x1 = std::min(n - 1, int32_t(std::ceil((hi.x - origin.x) * invStep)));
        const int32_t y1 = std::min(n - 1, int32_t(std::ceil((hi.y - origin.y) * invStep)));
        const int32_t z1 = std::min(n - 1, int32_t(std::ceil((hi.z - origin.z) * invStep)));
        for (int32_t z = z0; z <= z1; ++z)
            for (int32_t y = y0; y <= y1; ++y) {
                const size_t row = size_t(n) * (size_t(y) + size_t(n) * size_t(z));
                for (int32_t x = x0; x <= x1; ++x) {
                    const Vec3f p{origin.x + float(x) * step, origin.y + float(y) * step,
                                  origin.z + float(z) * step};
                    const float d = distance(p) - grow;
                    float& dst = out[row + size_t(x)];
                    if (d < dst) dst = d;
                }
            }
    };

    for (const CaveSegment& s : sys.segments) {
        const float pad = std::max(s.ra, s.rb) + grow + clampDist;
        rasterise(vmin(s.a, s.b) - Vec3f(pad), vmax(s.a, s.b) + Vec3f(pad),
                  [&](const Vec3f& p) { return capsuleDistance(p, s); });
    }
    for (const CaveChamber& c : sys.chambers) {
        const Vec3f pad = c.radii + Vec3f(grow + clampDist);
        rasterise(c.centre - pad, c.centre + pad, [&](const Vec3f& p) { return chamberDistance(p, c); });
    }
}

float caveDistance(const CaveSystem& sys, const Vec3f& p, float grow) {
    float d = 1e30f;
    for (const CaveSegment& s : sys.segments) d = std::min(d, capsuleDistance(p, s));
    for (const CaveChamber& c : sys.chambers) d = std::min(d, chamberDistance(p, c));
    return d - grow;
}

} // namespace vxe
