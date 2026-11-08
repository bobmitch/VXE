#include "vxe/world/CaveNetwork.h"

#include "vxe/core/Deterministic.h"

#include <algorithm>
#include <cmath>

namespace vxe {
namespace {

// A field standing in for how readily the rock gives way -- joint spacing,
// bedding, whatever is softer than its neighbours. One field does two jobs:
// it steers the passages while they are being walked, and it roughens their
// walls afterwards, so the wobble in a wall and the bend in the passage come
// from the same cause rather than looking like two effects stacked.
float valueNoise3(const Vec3f& p, int32_t seed) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int32_t ix = int32_t(fx), iy = int32_t(fy), iz = int32_t(fz);
    const float tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
    const float ux = tx * tx * (3.f - 2.f * tx), uy = ty * ty * (3.f - 2.f * ty), uz = tz * tz * (3.f - 2.f * tz);
    const auto corner = [&](int32_t dx, int32_t dy, int32_t dz) {
        return det::unit(det::hash3(ix + dx, iy + dy, iz + dz, seed));
    };
    const float c00 = corner(0, 0, 0) + (corner(1, 0, 0) - corner(0, 0, 0)) * ux;
    const float c10 = corner(0, 1, 0) + (corner(1, 1, 0) - corner(0, 1, 0)) * ux;
    const float c01 = corner(0, 0, 1) + (corner(1, 0, 1) - corner(0, 0, 1)) * ux;
    const float c11 = corner(0, 1, 1) + (corner(1, 1, 1) - corner(0, 1, 1)) * ux;
    const float c0 = c00 + (c10 - c00) * uy, c1 = c01 + (c11 - c01) * uy;
    return c0 + (c1 - c0) * uz;
}

// Signed, roughly [-1, 1]. `minWavelength` drops octaves finer than the samples
// can carry: a wall detail below the sample spacing is not detail, it is noise
// that changes shape at every LOD.
float rockField(const Vec3f& p, float scale, int32_t octaves, float minWavelength, int32_t seed) {
    float sum = 0.f, amp = 1.f, norm = 0.f, wavelength = scale;
    for (int32_t o = 0; o < octaves; ++o) {
        if (wavelength < minWavelength) break;
        sum += (valueNoise3(p * (1.f / wavelength), seed + o * 7717) * 2.f - 1.f) * amp;
        norm += amp;
        amp *= 0.52f;
        wavelength *= 0.45f;
    }
    return norm > 0.f ? sum / norm : 0.f;
}

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

// Value noise over the step index: correlated along the passage rather than
// jittering per segment, so the bore widens and pinches in runs.
float pathNoise(int32_t i, int32_t period, uint32_t salt) {
    const int32_t k = i / period;
    const float f = float(i - k * period) / float(period);
    const float a = det::unit(det::hash(uint32_t(k) * 0x9E3779B9u ^ salt));
    const float b = det::unit(det::hash(uint32_t(k + 1) * 0x9E3779B9u ^ salt));
    const float t = f * f * (3.f - 2.f * f);
    return a + (b - a) * t;
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
          int32_t branchDepth, float entranceY, float scale, const Aabbf& allowed, CaveSystem& sys) {
    Vec3f cur = from;
    float yaw = bearing;
    const uint32_t salt = rs.next();
    const float base = (p.tunnelRadius + rs.range(-p.tunnelRadiusVar, p.tunnelRadiusVar)) * scale;
    // Two periods of wander give runs of wide passage with pinches between,
    // which is most of the difference between a cave and a pipe.
    const auto radiusAt = [&](int32_t i) {
        const float w = pathNoise(i, 6, salt) * 0.62f + pathNoise(i, 2, salt ^ 0x5BD1u) * 0.38f;
        const float breathe = 1.f + p.radiusWander * (w * 2.f - 1.f);
        // Taper away over the last third, so a passage ends rather than stops.
        const float t = float(i) / float(std::max(1, steps));
        const float taper = t < 0.66f ? 1.f : 1.f - 0.72f * ((t - 0.66f) / 0.34f);
        return std::max(1.1f, base * breathe * taper);
    };
    float radius = radiusAt(0);

    for (int32_t i = 0; i < steps; ++i) {
        // Wander. Scaled by depth so the first few steps stay near the bearing
        // the surface handed us and the passage reads as going *in* somewhere.
        const float settle = std::min(1.f, float(i) * 0.25f);
        // The rock steers as well as roughens. Because this is a field rather
        // than a dice roll, passages in the same ground bend the same way, and
        // a route reads as following something instead of wandering at random.
        const float bend = rockField(cur, p.routeScale, 2, 0.f, 0x2B17) * p.routeChaos;
        yaw += (rs.range(-p.turnRate, p.turnRate) + bend) * settle;

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

        const float nextRadius = radiusAt(i + 1);
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
                const float r = (p.chamberRadius + rs.range(-p.chamberRadiusVar, p.chamberRadiusVar)) * scale;
                // Several overlapping lobes around the passage, each offset and
                // squashed differently. One ellipsoid reads as a bowl; a
                // cluster reads as a room with alcoves and a broken roof.
                for (int32_t l = 0; l < p.chamberLobes; ++l) {
                    const float lr = r * (l == 0 ? 1.f : rs.range(0.45f, 0.85f));
                    ch.centre = cur + Vec3f{rs.range(-r, r) * 0.7f, rs.range(-r, r) * 0.3f, rs.range(-r, r) * 0.7f} *
                                          (l == 0 ? 0.f : 1.f);
                    // Wider than tall: a room, not a bubble.
                    ch.radii = {lr * rs.range(0.9f, 1.5f), lr * rs.range(0.45f, 0.8f), lr * rs.range(0.9f, 1.5f)};
                    sys.chambers.push_back(ch);
                    growBounds(sys.bounds, ch.centre, std::max(ch.radii.x, std::max(ch.radii.y, ch.radii.z)));
                }
            }
        }
        if (branching) {
            const float turn = rs.range(0.6f, 1.5f) * (rs.chance(0.5f) ? 1.f : -1.f);
            walk(p, rs, cur, yaw + turn, descent * rs.range(0.5f, 1.2f), std::max(4, steps / 2), branchDepth + 1,
                 entranceY, scale, allowed, sys);
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
    // One size for the whole system, so length and bore and mouth agree: a long
    // system advertises itself, a short one can still be a surprise.
    const float scale = 1.f + rs.range(-p.sizeVariation, p.sizeVariation);
    const int32_t steps = std::max(6, int32_t(float(p.trunkSteps) * scale));
    const int32_t wanted = std::min(p.entrancesPerSystem, int32_t(basins.size()));
    for (int32_t e = 0; e < wanted; ++e) {
        const Candidate& c = basins[size_t(e)];
        // Sea floor and below is not where a walk-in entrance belongs.
        if (c.pos.y < 3.f) continue;

        // The mouth. Wider than the passage behind it and wide for a way in,
        // because a 3 m hole in a hillside could not be found even with a
        // marker standing over it. It starts above the surface so the opening
        // is cut rather than sealed under a last metre of rock.
        const float bore = p.tunnelRadius * scale;
        const float flare = bore * p.entranceFlare;
        const float bearing = std::atan2(c.downhill.z, c.downhill.x);
        const Vec3f mouth{c.pos.x, c.pos.y + flare * 0.55f, c.pos.z};
        const float run = p.throatLength * scale;
        const Vec3f waist{c.pos.x + c.downhill.x * run, c.pos.y - run * 0.35f, c.pos.z + c.downhill.z * run};
        const Vec3f inside{c.pos.x + c.downhill.x * run * 1.9f, c.pos.y - run * 0.95f,
                           c.pos.z + c.downhill.z * run * 1.9f};

        // Two segments: a flared opening that holds its width, then the
        // narrowing into the passage proper.
        sys.segments.push_back({mouth, waist, flare, flare * 0.8f});
        sys.segments.push_back({waist, inside, flare * 0.8f, bore});
        sys.entrances.push_back(mouth);
        growBounds(sys.bounds, mouth, flare);
        growBounds(sys.bounds, waist, flare);
        growBounds(sys.bounds, inside, bore);

        walk(p, rs, inside, bearing, p.descentRate, steps, 0, c.pos.y, scale, allowed, sys);
    }
    return sys;
}

void splatCaves(const CaveParams& cp, const CaveSystem& sys, const Vec3f& origin, float step, int32_t n,
                float grow, float clampDist, float* out) {
    const float invStep = 1.f / step;
    // Finest wall detail the samples can carry. Below this it is not detail,
    // it is a different shape at every LOD.
    const float minWave = step * 2.2f;
    const auto rasterise = [&](const Vec3f& lo, const Vec3f& hi, float localRadius, auto&& distance) {
        const float rough = localRadius * cp.wallRoughness;
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
                    float d = distance(p) - grow;
                    // Only the shell around the wall needs the field; deep rock
                    // and open air are decided already, and the noise is the
                    // expensive part of this loop.
                    if (rough > 0.01f && d > -rough - step && d < rough + step)
                        d += rockField(p, cp.wallScale, cp.wallOctaves, minWave, 0x51F) * rough;
                    float& dst = out[row + size_t(x)];
                    if (d < dst) dst = d;
                }
            }
    };

    for (const CaveSegment& s : sys.segments) {
        const float r = std::max(s.ra, s.rb);
        const float pad = r * (1.f + cp.wallRoughness) + grow + clampDist;
        rasterise(vmin(s.a, s.b) - Vec3f(pad), vmax(s.a, s.b) + Vec3f(pad), r,
                  [&](const Vec3f& p) { return capsuleDistance(p, s); });
    }
    for (const CaveChamber& c : sys.chambers) {
        const float r = std::min(c.radii.x, std::min(c.radii.y, c.radii.z));
        const Vec3f pad = c.radii * (1.f + cp.wallRoughness) + Vec3f(grow + clampDist);
        rasterise(c.centre - pad, c.centre + pad, r,
                  [&](const Vec3f& p) { return chamberDistance(p, c); });
    }
}

float caveDistance(const CaveParams& cp, const CaveSystem& sys, const Vec3f& p, float grow, float sampleStep) {
    // Tracks the splat: nearest primitive first, then that primitive's own
    // radius sets how much the wall may wander. Edits and queries have to see
    // the same wall the mesher did.
    float d = 1e30f, radius = 0.f;
    for (const CaveSegment& s : sys.segments) {
        const float c = capsuleDistance(p, s);
        if (c < d) { d = c; radius = std::max(s.ra, s.rb); }
    }
    for (const CaveChamber& c : sys.chambers) {
        const float e = chamberDistance(p, c);
        if (e < d) { d = e; radius = std::min(c.radii.x, std::min(c.radii.y, c.radii.z)); }
    }
    d -= grow;
    const float rough = radius * cp.wallRoughness;
    if (rough > 0.01f && d > -rough - sampleStep && d < rough + sampleStep)
        d += rockField(p, cp.wallScale, cp.wallOctaves, sampleStep * 2.2f, 0x51F) * rough;
    return d;
}

} // namespace vxe
