#pragma once
// Minimal value-type math used by engine-side systems. Library boundaries
// (Jolt, ozz, FastNoise2) convert to their own SIMD types at the call site;
// we deliberately do not leak third-party math types through engine headers.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

namespace vxe {

template <typename T>
struct Vec3T {
    T x{}, y{}, z{};

    constexpr Vec3T() = default;
    constexpr Vec3T(T x_, T y_, T z_) : x(x_), y(y_), z(z_) {}
    constexpr explicit Vec3T(T s) : x(s), y(s), z(s) {}
    template <typename U>
    constexpr explicit Vec3T(const Vec3T<U>& o) : x(T(o.x)), y(T(o.y)), z(T(o.z)) {}

    constexpr T& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
    constexpr const T& operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }

    constexpr Vec3T operator+(const Vec3T& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3T operator-(const Vec3T& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3T operator*(const Vec3T& o) const { return {x * o.x, y * o.y, z * o.z}; }
    constexpr Vec3T operator*(T s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3T operator/(T s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3T operator-() const { return {-x, -y, -z}; }
    constexpr Vec3T& operator+=(const Vec3T& o) { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr Vec3T& operator-=(const Vec3T& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    constexpr Vec3T& operator*=(T s) { x *= s; y *= s; z *= s; return *this; }
    constexpr bool operator==(const Vec3T& o) const = default;
};

using Vec3f = Vec3T<float>;
using Vec3d = Vec3T<double>;
using Vec3i = Vec3T<int32_t>;

template <typename T> constexpr T dot(const Vec3T<T>& a, const Vec3T<T>& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
template <typename T> constexpr Vec3T<T> cross(const Vec3T<T>& a, const Vec3T<T>& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
template <typename T> inline T length(const Vec3T<T>& v) { return std::sqrt(dot(v, v)); }
template <typename T> inline Vec3T<T> normalize(const Vec3T<T>& v) {
    const T l = length(v);
    return l > T(1e-20) ? v / l : Vec3T<T>(T(0), T(1), T(0));
}
template <typename T> constexpr Vec3T<T> vmin(const Vec3T<T>& a, const Vec3T<T>& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
template <typename T> constexpr Vec3T<T> vmax(const Vec3T<T>& a, const Vec3T<T>& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
template <typename T> constexpr Vec3T<T> vclamp(const Vec3T<T>& v, const Vec3T<T>& lo, const Vec3T<T>& hi) { return vmin(vmax(v, lo), hi); }

inline Vec3i floorToInt(const Vec3d& v) {
    return {int32_t(std::floor(v.x)), int32_t(std::floor(v.y)), int32_t(std::floor(v.z))};
}

// Floor division that rounds toward -inf (C++ '/' truncates toward zero).
constexpr int32_t floorDiv(int32_t a, int32_t b) {
    const int32_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
constexpr int32_t floorMod(int32_t a, int32_t b) { return a - floorDiv(a, b) * b; }

template <typename T>
struct AabbT {
    Vec3T<T> min{}, max{};
    constexpr bool intersects(const AabbT& o) const {
        return min.x <= o.max.x && max.x >= o.min.x && min.y <= o.max.y && max.y >= o.min.y &&
               min.z <= o.max.z && max.z >= o.min.z;
    }
    constexpr bool contains(const Vec3T<T>& p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
    }
    constexpr AabbT expanded(T r) const { return {min - Vec3T<T>(r), max + Vec3T<T>(r)}; }
    // Distance from a point to the box (0 if inside).
    T distanceTo(const Vec3T<T>& p) const {
        const Vec3T<T> d = vmax(vmax(min - p, p - max), Vec3T<T>(T(0)));
        return length(d);
    }
};
using Aabbf = AabbT<float>;
using Aabbd = AabbT<double>;
using Aabbi = AabbT<int32_t>;

constexpr float saturate(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
constexpr float lerp(float a, float b, float t) { return a + (b - a) * t; }
constexpr float smoothstep(float e0, float e1, float x) {
    const float t = saturate((x - e0) / (e1 - e0));
    return t * t * (3.f - 2.f * t);
}

// 64-bit mixing hash (splitmix64 finaliser) - used by spatial hash maps.
constexpr uint64_t mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

struct Vec3iHash {
    size_t operator()(const Vec3i& v) const noexcept {
        const uint64_t k = (uint64_t(uint32_t(v.x)) * 73856093ull) ^ (uint64_t(uint32_t(v.y)) * 19349663ull << 1) ^
                           (uint64_t(uint32_t(v.z)) * 83492791ull << 2);
        return size_t(mix64(k));
    }
};

} // namespace vxe
