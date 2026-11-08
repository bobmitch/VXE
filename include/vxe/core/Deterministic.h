#pragma once
// -----------------------------------------------------------------------------
// Deterministic.h - arithmetic that is guaranteed to produce the same bits on
// every machine, which §8 of docs/world-design.md makes load-bearing: DeltaStore
// records edits relative to regenerated base density, and co-op has two clients
// regenerate the same world independently. Either of those breaks silently if
// generation drifts by one ulp.
//
// std::sin and friends are not pinned to a bit pattern by any standard, and do
// differ between libms and optimisation levels, so anything in the generator's
// path uses the polynomials here instead. Integer hashing replaces any RNG, so
// a value depends on its coordinates rather than on how many times something
// was called before it.
// -----------------------------------------------------------------------------

#include <cmath>
#include <cstdint>

namespace vxe::det {

// Odd 7th-order minimax sine on [-pi, pi]. std::floor is exact, and every
// operation below is IEEE float arithmetic in a fixed order.
inline float sin(float x) {
    constexpr float kInvTwoPi = 0.15915494309189535f;
    constexpr float kTwoPi    = 6.283185307179586f;
    x -= kTwoPi * std::floor(x * kInvTwoPi + 0.5f);
    const float x2 = x * x;
    return x * (1.f + x2 * (-0.16666667f + x2 * (0.0083333310f + x2 * -0.00019840874f)));
}
inline float cos(float x) { return det::sin(x + 1.5707963267948966f); }

inline uint32_t hash(uint32_t h) {
    h ^= h >> 15; h *= 0x2C1B3C6Du;
    h ^= h >> 12; h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}
inline uint32_t hash2(int32_t x, int32_t y, int32_t seed) {
    return det::hash(uint32_t(x) * 0x8DA6B343u ^ uint32_t(y) * 0xD8163841u ^ uint32_t(seed) * 0xCB1AB31Fu);
}
inline uint32_t hash3(int32_t x, int32_t y, int32_t z, int32_t seed) {
    return det::hash(uint32_t(x) * 0x8DA6B343u ^ uint32_t(y) * 0xD8163841u ^ uint32_t(z) * 0x1B873593u ^
                     uint32_t(seed) * 0xCB1AB31Fu);
}

// [0,1) from a hash, and a cheap stream of them for walk-style generation.
inline float unit(uint32_t h) { return float(h >> 8) * (1.f / 16777216.f); }

// A deterministic sequence: step() advances, so order matters but the starting
// point is a pure function of the coordinates that seeded it.
struct Stream {
    uint32_t s;
    explicit Stream(uint32_t seed) : s(det::hash(seed | 1u)) {}
    uint32_t next() { s = det::hash(s + 0x9E3779B9u); return s; }
    float unit() { return det::unit(next()); }                       // [0, 1)
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
    bool chance(float p) { return unit() < p; }
};

} // namespace vxe::det
