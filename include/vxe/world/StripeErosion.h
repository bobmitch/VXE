#pragma once
// -----------------------------------------------------------------------------
// StripeErosion: gullies and ridges that follow the slope, as a filter over any
// height function.
//
// The idea is not ours. Water runs downhill, so if stripes are laid along the
// negative gradient they read as the gullies water would have cut, and each
// octave's stripes follow the slopes the previous octave left -- which is what
// makes small gullies branch off larger ones at an angle instead of crossing
// them. See Rune Vision's write-up (blog.runevision.com, March 2026); the
// slope-proportional-frequency variant there is credited to Clay John and
// Fewes. This is the fade variant: stripe width stays fixed and stripes fade
// out where the ground is flat.
//
// Why it suits this generator: it is a per-point function of (position, height
// gradient), which is the same shape as the column pass already running on
// 38x38 columns per chunk. Nothing downstream changes -- density, meshing,
// collision and the far-field trace all consume the height it produces.
//
// Simplified against the published technique: no stacked fading, no gully
// normalisation, no straight-gully correction, no drainage lines. Those sharpen
// the look; none of them change how it plugs in.
//
// Determinism: §8 makes bit-identical regeneration load-bearing, and std::sin
// is not guaranteed identical across platforms or compilers, so this uses a
// polynomial sine and an integer hash. Every operation here is IEEE float
// arithmetic in a fixed order.
// -----------------------------------------------------------------------------

#include "vxe/core/Math.h"

#include <cmath>
#include <cstdint>

namespace vxe {

struct StripeParams {
    int32_t octaves     = 4;
    float   cellSize    = 240.f;   // metres: pivot cell for the first octave's stripes
    float   stripeWidth = 60.f;    // metres from gully to gully, first octave
    float   amplitude   = 5.f;     // metres of relief, first octave
    float   lacunarity  = 0.5f;    // size multiplier per octave
    // Gain must stay below lacunarity. A gully's steepness goes as
    // amplitude/width, so at gain == lacunarity every octave is as steep as the
    // last and at gain > lacunarity they get steeper as they get smaller --
    // which turns the whole surface into spikes rather than gullies.
    float   gain        = 0.4f;    // amplitude multiplier per octave
    float   slopeFeed   = 0.6f;    // how far a gully's own slope steers the next octave
    float   slopeRef    = 0.18f;   // gradient magnitude at which stripes reach full strength
    // Stacked fading. Without it each octave lays its gullies wherever the
    // slope allows, including along the crest of the ridge the previous octave
    // just raised, and the surface reads as lumps rather than drainage. This
    // biases each octave toward the creases the last one cut: 0 disables it,
    // 1 puts detail only in creases.
    float   detailBias  = 0.7f;
    // Straight gullies. Feeding the raw sine back as the stripe's slope makes
    // gullies curl away at their ends, because the perturbation dies off just
    // where the gully should keep running. Blending toward its sign keeps the
    // turn constant along the stripe so gullies run straight down the slope.
    float   straightness = 0.7f;
    // Distance over which the incoming gradient is measured. Gullies follow the
    // hillside, not the bumps on it: at a few metres the direction swings with
    // every small feature and the stripes never line up into anything. This
    // wants to be comparable with the first octave's stripe width.
    float   gradEps     = 48.f;
    int32_t seed        = 1337;
};

// Height gradient, metres per metre.
struct SlopeXZ { float dx = 0.f, dz = 0.f; };

// The most the filter can move a column, either way. Culling margins and
// height envelopes are computed from the unfiltered height, so they have to
// allow for this or a chunk the erosion pushed into view gets skipped.
inline float stripeMaxOffset(const StripeParams& sp) {
    float amp = sp.amplitude, total = 0.f;
    for (int32_t o = 0; o < sp.octaves; ++o) { total += amp; amp *= sp.gain; }
    return total;
}

namespace stripe_detail {

// Polynomial sine, in place of std::sin, which no standard pins to a bit
// pattern. Odd 7th-order minimax on [-pi, pi]; std::floor is exact.
inline float detSin(float x) {
    constexpr float kInvTwoPi = 0.15915494309189535f;
    constexpr float kTwoPi    = 6.283185307179586f;
    x -= kTwoPi * std::floor(x * kInvTwoPi + 0.5f);
    const float x2 = x * x;
    return x * (1.f + x2 * (-0.16666667f + x2 * (0.0083333310f + x2 * -0.00019840874f)));
}
inline float detCos(float x) { return detSin(x + 1.5707963267948966f); }

inline uint32_t hashCell(int32_t cx, int32_t cz, int32_t seed) {
    uint32_t h = uint32_t(cx) * 0x8DA6B343u ^ uint32_t(cz) * 0xD8163841u ^ uint32_t(seed) * 0xCB1AB31Fu;
    h ^= h >> 15; h *= 0x2C1B3C6Du;
    h ^= h >> 12; h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}
// Two independent [0,1) values from one cell.
inline void cellPivot(int32_t cx, int32_t cz, int32_t seed, float& u, float& v) {
    const uint32_t h = hashCell(cx, cz, seed);
    u = float(h & 0xFFFFu) * (1.f / 65536.f);
    v = float((h >> 16) & 0xFFFFu) * (1.f / 65536.f);
}

} // namespace stripe_detail

// Returns the height offset in metres and advances `grad` to the eroded
// gradient. `grad` must come in as the gradient of the base height function,
// measured at a fixed world-space epsilon -- not at the chunk's sample spacing,
// or the gully pattern would change shape at every LOD and pop when it did.
// `step` is the chunk's sample spacing. A stripe narrower than two samples
// across is not a gully at that LOD, it is noise riding on the height, and it
// lands wherever the lattice happens to cut it -- so the finer octaves fade
// out as the samples spread. LOD0 passes 1 and keeps all of them exactly.
inline float stripeErosion(const StripeParams& sp, float x, float z, SlopeXZ& grad, float step = 1.f) {
    using namespace stripe_detail;
    constexpr float kTwoPi = 6.283185307179586f;

    float offset = 0.f;
    float cell = sp.cellSize, width = sp.stripeWidth, amp = sp.amplitude;
    SlopeXZ g = grad;
    // 1 where the previous octave left a crease, 0 along its ridge. Starts open
    // so the first octave is unconstrained.
    float creaseMask = 1.f;

    for (int32_t o = 0; o < sp.octaves; ++o) {
        const float len = std::sqrt(g.dx * g.dx + g.dz * g.dz);
        // Flat ground has no flow direction, so stripes there would fold into
        // chaos around peaks and valleys. Fade them out instead.
        const float steep = smoothstep(0.15f * sp.slopeRef, sp.slopeRef, len);
        const float band = step <= 1.f ? 1.f : smoothstep(2.f * step, 4.f * step, width);
        const float fade = steep * lerp(1.f - sp.detailBias, 1.f, creaseMask) * band;
        if (fade > 0.001f) {
            // Downhill, and the across-flow axis the stripes vary along.
            const float inv = 1.f / len;
            const float dirX = -g.dx * inv, dirZ = -g.dz * inv;
            const float perpX = -dirZ, perpZ = dirX;

            // Rotating one global pattern about a single pivot distorts further
            // from it, so each cell carries its own pivot and the four around a
            // point are blended. Two unaligned cosines average to a cosine of
            // smaller amplitude, which is why the joins do not show.
            const float invCell = 1.f / cell;
            const float cxf = x * invCell, czf = z * invCell;
            const int32_t bx = int32_t(std::floor(cxf)), bz = int32_t(std::floor(czf));
            const float fx = cxf - float(bx), fz = czf - float(bz);

            float waveSum = 0.f, slopeSum = 0.f, wSum = 0.f;
            for (int32_t j = 0; j <= 1; ++j)
                for (int32_t i = 0; i <= 1; ++i) {
                    const int32_t cx = bx + i, cz = bz + j;
                    float pu, pv;
                    cellPivot(cx, cz, sp.seed + o * 7919, pu, pv);
                    const float px = (float(cx) + pu) * cell, pz = (float(cz) + pv) * cell;
                    // Stripe phase: distance across the flow from this pivot.
                    const float s = ((x - px) * perpX + (z - pz) * perpZ) * (kTwoPi / width);
                    const float w = (i ? fx : 1.f - fx) * (j ? fz : 1.f - fz);
                    waveSum  += w * detCos(s);
                    slopeSum += w * detSin(s);
                    wSum     += w;
                }
            const float invW = wSum > 1e-6f ? 1.f / wSum : 0.f;
            const float wave = waveSum * invW, slope = slopeSum * invW;

            offset += amp * wave * fade;
            // The stripe varies only across the flow, so its slope is along
            // perp. Feeding it back is what makes the next octave branch --
            // but it has to perturb the flow direction, not replace it. Left
            // unbounded, a steep gully's own slope dwarfs the hillside's and
            // the next octave runs its stripes across this one instead of
            // down it, which cross-hatches the surface into chaos.
            const float slopeSign = slope >= 0.f ? 1.f : -1.f;
            const float slopeEff = lerp(slope, slopeSign, sp.straightness);
            const float maxTurn = len * sp.slopeFeed;
            float dAcross = -amp * slopeEff * (kTwoPi / width) * fade * sp.slopeFeed;
            dAcross = dAcross < -maxTurn ? -maxTurn : (dAcross > maxTurn ? maxTurn : dAcross);
            g.dx += perpX * dAcross;
            g.dz += perpZ * dAcross;

            // Hand the next octave this one's creases.
            creaseMask = saturate(0.5f - 0.5f * wave);
        } else {
            creaseMask = 1.f;
        }
        cell *= sp.lacunarity;
        width *= sp.lacunarity;
        amp *= sp.gain;
    }

    grad = g;
    return offset;
}

} // namespace vxe
