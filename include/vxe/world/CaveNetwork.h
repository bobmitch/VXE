#pragma once
// -----------------------------------------------------------------------------
// CaveNetwork: caves as placed geometry, routed by where water would go.
//
// The noise caves this replaces left about 18% of all subsurface rock hollow,
// uniformly, everywhere, and could not have done otherwise: a threshold on a
// stationary field has no way to make one place cavey and another solid. See
// §9 of docs/world-design.md for the five separate arguments that landed on
// the same conclusion.
//
// What is placed here instead:
//
//   Region   A cave system exists in a region cell or it does not, and most do
//            not. One system per cell, contained within it, so cells never have
//            to agree with each other about anything.
//
//   Entrance Karst is water dissolving rock, so entrances go where water
//            collects: the low, flat places the surface drains into. Those are
//            found by probing the height field the terrain generator already
//            has, which is what ties the underground to the landscape above it
//            -- a player who can read a valley can find a cave mouth, and §7's
//            legibility problem is answered by geography instead of signage.
//
//   Route    From the entrance the trunk walks downhill, the way water would,
//            turning and branching as it descends toward a discharge level.
//
//   Form     Capsules and ellipsoids, composed into density by SDF. Analytic,
//            so a tunnel evaluates at any LOD instead of vanishing past LOD1,
//            and a radius can be widened to stay sampleable when the samples
//            spread out. Thresholded noise could do neither.
//
// Placement is plausible; the spaces themselves need not be. §9 scopes §1's
// grounded rule to the surface, so chambers can be larger than rock mechanics
// allow and floors can cooperate. What is earned here is *where*, not *what*.
//
// Determinism: integer hashes and the polynomials in core/Deterministic.h, so a
// system is a pure function of its cell and the world seed, generated the same
// on any machine, in any order, on any thread.
// -----------------------------------------------------------------------------

#include "vxe/core/Math.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace vxe {

// How far beyond its own cell a system is allowed to wander, as a fraction of
// the cell. Chunks widen their cell query by the same amount, so a passage can
// never reach a chunk that would not have asked about it.
inline constexpr float kCellSlack = 0.3f;

struct CaveParams {
    // Region cell. One system per cell at most, so this is also roughly the
    // spacing between separate cave systems.
    float regionSize = 1024.f;
    // Fraction of cells holding a system. Most of the world has no caves at
    // all, which is the whole correction over the noise fields.
    float regionDensity = 0.3f;

    // Passages are deliberately wider than a person needs. At 1 m samples a
    // 3 m passage is six voxels across, and no amount of noise makes six
    // voxels look like rock -- the detail that would sell it is finer than the
    // thing it is carved into. Games oversize their caves for exactly this
    // reason, and it reads as natural rather than generous.
    float tunnelRadius = 5.0f;       // metres, mean; individual passages vary
    float tunnelRadiusVar = 1.7f;
    float stepLength = 26.f;         // metres between route vertices
    int32_t trunkSteps = 22;         // how far a trunk runs before it stops
    int32_t maxBranchDepth = 2;      // branches off branches, and no further
    float branchChance = 0.22f;      // per step
    float turnRate = 0.45f;          // radians of wander per step
    float descentRate = 0.34f;       // metres down per metre along
    float maxDepth = 190.f;          // below the entrance

    // A passage closes down as it descends. The water that cut it had less and
    // less to work with the further it went, and a cave that held the same
    // bore 200 m down reads as a corridor someone built. Depth drives it, with
    // a little from sheer distance so a passage that flattens out at depth
    // still ends rather than running on.
    float depthSqueeze = 0.0085f;    // fraction of bore lost per metre descended
    float squeezeFloor = 0.22f;      // tightest it goes before the passage ends
    // A chamber reopens what lies beyond it -- a junction is fed from
    // somewhere else, so the squeeze starts again rather than carrying on from
    // a crawl. But never back to what it was: each chamber starts a run that
    // is a seeded fraction of the one before, so a system steps down through
    // its chambers and ends up somewhere it cannot go on from. A full reset
    // would let a system ladder its way to any depth at full bore.
    float chamberReliefMin = 0.5f;
    float chamberReliefMax = 0.82f;

    float chamberChance = 0.3f;      // per branch point
    float chamberRadius = 18.f;
    float chamberRadiusVar = 9.f;
    // A chamber is a cluster of overlapping lobes rather than one ellipsoid.
    // Unioned by min() they read as a room with alcoves instead of a bowl.
    int32_t chamberLobes = 4;

    // Systems come in sizes, drawn once per system and applied to length,
    // passage width and mouth alike -- so a long system announces itself with a
    // big opening and a short one can still be a surprise.
    float sizeVariation = 0.55f;     // +/- this fraction around 1

    // A mouth is wider than the passage behind it, and stays wide for a little
    // way in. Without this an entrance is a 3 m hole in a hillside, which in
    // practice could not be found even with a marker standing over it.
    float entranceFlare = 2.8f;      // mouth radius as a multiple of the passage
    float throatLength = 18.f;       // metres of flared opening before it narrows

    // How much the passage radius breathes along its length. Constrictions and
    // widenings, rather than a tube of constant bore.
    float radiusWander = 0.6f;

    // Wall roughness. A capsule is a machined tube and an ellipsoid is a
    // sphere; what makes either read as rock is that the wall itself is
    // irregular. Displacement is a fraction of the *local* radius, so a wide
    // chamber gets metres of relief and a narrow passage gets centimetres and
    // does not pinch itself shut.
    float wallRoughness = 0.36f;
    // Feature size is a multiple of the *local* radius, not a fixed distance.
    // A fixed 26 m coarsest octave meant a 3 m passage was modulated by
    // features longer than it was wide, which is a gentle bend, not texture.
    // Relative scale gives every passage the same number of features across
    // its bore whatever its size -- which is what self-similar means.
    float wallScaleFactor = 2.0f;    // coarsest octave, in local radii
    int32_t wallOctaves = 4;
    // Rock is bedded, and the ledges and shelves that reads as are the single
    // strongest cue that a wall is stone rather than a mesh. Sampling the
    // field squashed in Y makes its features wide and flat.
    float wallBedding = 1.8f;

    // How far the rock field bends a passage as it is walked. Without it the
    // route is a pure random walk, which reads as smooth because nothing
    // correlates between one step and the next.
    float routeChaos = 0.95f;
    float routeScale = 220.f;        // metres per bend

    // Entrance search: candidates per cell, and how much lower than the cell's
    // mean a candidate has to sit before it counts as somewhere water collects.
    int32_t entranceCandidates = 24;
    int32_t entrancesPerSystem = 2;
    float bedrockY = -440.f;
};

// The widest anything in a system can be: a mouth, on the largest system size.
// Chunk queries add it to their reach, because a cell is asked about by
// distance to the cell, and a fat segment at the edge of its allowance reaches
// further than the cell does.
inline float maxCaveRadius(const CaveParams& p) {
    return p.tunnelRadius * p.entranceFlare * (1.f + p.sizeVariation);
}

// One capsule of passage. Radii at each end let a passage taper and let a
// chamber mouth open out of it.
struct CaveSegment {
    Vec3f a{}, b{};
    float ra = 3.f, rb = 3.f;
};

struct CaveChamber {
    Vec3f centre{};
    Vec3f radii{};
};

struct CaveSystem {
    std::vector<CaveSegment> segments;
    std::vector<CaveChamber> chambers;
    // The mouths, kept separately: they are the only part of a system the
    // surface can see, so anything asking "is there a way in near here" wants
    // these rather than the 200 passages behind them.
    std::vector<Vec3f> entrances;
    Aabbf bounds{Vec3f(1e30f), Vec3f(-1e30f)};
    bool empty() const { return segments.empty() && chambers.empty(); }
};

// Surface height in metres at a world position. The generator supplies its own.
using HeightFn = std::function<float(float x, float z)>;

// Builds the system for one region cell, or an empty one if that cell has no
// caves. Pure function of (seed, cell) given the same height field.
CaveSystem buildCaveSystem(const CaveParams& p, int32_t seed, int32_t cellX, int32_t cellZ, const HeightFn& height);

// Signed distance to the nearest passage or chamber, negative inside. `grow`
// widens everything by that many metres, which is how a passage narrower than
// the sample spacing is kept alive at coarse LOD.
float caveDistance(const CaveParams& cp, const CaveSystem& sys, const Vec3f& p, float grow, float sampleStep);

// Rasterises the system into `out`, an n^3 field of signed distances indexed
// x + n*(y + n*z), keeping the minimum where it already holds something nearer.
// Only samples within `clampDist` of a passage are touched, so the cost follows
// how much cave is in the chunk rather than how many samples the chunk has --
// which matters because most chunks contain none at all.
void splatCaves(const CaveParams& cp, const CaveSystem& sys, const Vec3f& origin, float step, int32_t n,
                float grow, float clampDist, float* out);

} // namespace vxe
