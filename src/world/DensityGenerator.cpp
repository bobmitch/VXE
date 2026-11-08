#include "vxe/world/DensityGenerator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#if VXE_WITH_FASTNOISE2
#include <FastNoise/FastNoise.h>
#endif

namespace vxe {

namespace {

constexpr int32_t S = kFieldSize;
constexpr int32_t kCols = S * S;

// Continental height spline: (continentalness, height in metres).
struct SplinePoint { float c, h; };
constexpr std::array<SplinePoint, 7> kContinentalSpline{{
    {-1.00f, -90.f}, {-0.35f, -55.f}, {-0.12f, -6.f}, {-0.04f, 2.f},
    { 0.25f,  28.f}, { 0.55f,  85.f}, { 1.00f, 170.f},
}};

float evalSpline(float c) {
    if (c <= kContinentalSpline.front().c) return kContinentalSpline.front().h;
    for (size_t i = 1; i < kContinentalSpline.size(); ++i) {
        const auto& a = kContinentalSpline[i - 1];
        const auto& b = kContinentalSpline[i];
        if (c <= b.c) {
            const float t = (c - a.c) / (b.c - a.c);
            const float s = t * t * (3.f - 2.f * t); // C1 smooth between knots
            return lerp(a.h, b.h, s);
        }
    }
    return kContinentalSpline.back().h;
}

// ---------------------------------------------------------------------------
// Noise backend. FastNoise2 is the production path; the software fallback
// exists so the engine library builds with VXE_WITH_FASTNOISE2=OFF (CI, ports).
// ---------------------------------------------------------------------------
#if VXE_WITH_FASTNOISE2
class NoiseNode {
public:
    NoiseNode() = default;
    explicit NoiseNode(FastNoise::SmartNode<> n, int seedOffset) : m_node(std::move(n)), m_seedOffset(seedOffset) {}

    void grid2D(float* out, float x0, float z0, int count, float step, int seed) const {
        m_node->GenUniformGrid2D(out, x0, z0, count, count, step, step, seed + m_seedOffset);
    }
    void grid3D(float* out, float x0, float y0, float z0, int count, float step, int seed) const {
        m_node->GenUniformGrid3D(out, x0, y0, z0, count, count, count, step, step, step, seed + m_seedOffset);
    }
    float single2D(float x, float z, int seed) const { return m_node->GenSingle2D(x, z, seed + m_seedOffset); }
    float single3D(float x, float y, float z, int seed) const { return m_node->GenSingle3D(x, y, z, seed + m_seedOffset); }

private:
    FastNoise::SmartNode<> m_node;
    int m_seedOffset = 0;
};

FastNoise::SmartNode<> makeFbm(float scale, int octaves, float gain = 0.5f, float lacunarity = 2.0f) {
    auto simplex = FastNoise::New<FastNoise::Simplex>();
    simplex->SetScale(scale);
    auto fbm = FastNoise::New<FastNoise::FractalFBm>();
    fbm->SetSource(simplex);
    fbm->SetOctaveCount(octaves);
    fbm->SetGain(gain);
    fbm->SetLacunarity(lacunarity);
    return fbm;
}
FastNoise::SmartNode<> makeRidged(float scale, int octaves) {
    auto simplex = FastNoise::New<FastNoise::Simplex>();
    simplex->SetScale(scale);
    auto ridged = FastNoise::New<FastNoise::FractalRidged>();
    ridged->SetSource(simplex);
    ridged->SetOctaveCount(octaves);
    ridged->SetGain(0.5f);
    ridged->SetWeightedStrength(0.6f); // smoother valleys, sharp crests
    return ridged;
}
FastNoise::SmartNode<> makeSimplex(float scale) {
    auto simplex = FastNoise::New<FastNoise::Simplex>();
    simplex->SetScale(scale);
    return simplex;
}
#else
// Deterministic scalar gradient noise (Perlin-style quintic), range ~[-1, 1].
uint32_t hash3(int32_t x, int32_t y, int32_t z, int32_t seed) {
    uint32_t h = uint32_t(seed) * 0x27d4eb2du;
    h ^= uint32_t(x) * 0x9E3779B1u; h = (h << 13) | (h >> 19);
    h ^= uint32_t(y) * 0x85EBCA77u; h = (h << 13) | (h >> 19);
    h ^= uint32_t(z) * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
float gradDot(uint32_t h, float x, float y, float z) {
    static constexpr float g[12][3] = {{1,1,0},{-1,1,0},{1,-1,0},{-1,-1,0},{1,0,1},{-1,0,1},
                                       {1,0,-1},{-1,0,-1},{0,1,1},{0,-1,1},{0,1,-1},{0,-1,-1}};
    const float* v = g[h % 12];
    return v[0] * x + v[1] * y + v[2] * z;
}
float quintic(float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }
float gradientNoise(float x, float y, float z, int32_t seed) {
    const int32_t xi = int32_t(std::floor(x)), yi = int32_t(std::floor(y)), zi = int32_t(std::floor(z));
    const float xf = x - float(xi), yf = y - float(yi), zf = z - float(zi);
    const float u = quintic(xf), v = quintic(yf), w = quintic(zf);
    float c[8];
    for (int i = 0; i < 8; ++i) {
        const int dx = i & 1, dy = (i >> 1) & 1, dz = (i >> 2) & 1;
        c[i] = gradDot(hash3(xi + dx, yi + dy, zi + dz, seed), xf - float(dx), yf - float(dy), zf - float(dz));
    }
    const float x00 = lerp(c[0], c[1], u), x10 = lerp(c[2], c[3], u);
    const float x01 = lerp(c[4], c[5], u), x11 = lerp(c[6], c[7], u);
    return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w) * 0.97f;
}
enum class Kind { Simplex, FBm, Ridged };
class NoiseNode {
public:
    NoiseNode() = default;
    NoiseNode(Kind k, float scale, int octaves, int seedOffset) : m_kind(k), m_freq(1.f / scale), m_oct(octaves), m_seedOffset(seedOffset) {}
    float eval(float x, float y, float z, int seed) const {
        seed += m_seedOffset;
        x *= m_freq; y *= m_freq; z *= m_freq;
        if (m_kind == Kind::Simplex) return gradientNoise(x, y, z, seed);
        float sum = 0.f, amp = 1.f, norm = 0.f;
        for (int o = 0; o < m_oct; ++o) {
            float n = gradientNoise(x, y, z, seed + o);
            if (m_kind == Kind::Ridged) n = 1.f - 2.f * std::abs(n);
            sum += n * amp; norm += amp; amp *= 0.5f;
            x *= 2.f; y *= 2.f; z *= 2.f;
        }
        return sum / norm;
    }
    void grid2D(float* out, float x0, float z0, int count, float step, int seed) const {
        for (int j = 0; j < count; ++j)
            for (int i = 0; i < count; ++i) out[i + j * count] = eval(x0 + float(i) * step, 0.f, z0 + float(j) * step, seed);
    }
    void grid3D(float* out, float x0, float y0, float z0, int count, float step, int seed) const {
        for (int k = 0; k < count; ++k)
            for (int j = 0; j < count; ++j)
                for (int i = 0; i < count; ++i)
                    out[i + count * (j + count * k)] = eval(x0 + float(i) * step, y0 + float(j) * step, z0 + float(k) * step, seed);
    }
    float single2D(float x, float z, int seed) const { return eval(x, 0.f, z, seed); }
    float single3D(float x, float y, float z, int seed) const { return eval(x, y, z, seed); }

private:
    Kind m_kind = Kind::Simplex;
    float m_freq = 1.f;
    int m_oct = 1;
    int m_seedOffset = 0;
};
#endif

struct Scratch {
    std::vector<float> continental, mountainMask, ridge, hills, entrance;
    std::vector<float> height, mask01, entrance01;
    std::vector<float> overhang, caveA, caveB, cheese;
    Scratch()
        : continental(kCols), mountainMask(kCols), ridge(kCols), hills(kCols), entrance(kCols),
          height(kCols), mask01(kCols), entrance01(kCols),
          overhang(size_t(kFieldVolume)), caveA(size_t(kFieldVolume)), caveB(size_t(kFieldVolume)),
          cheese(size_t(kFieldVolume)) {}
};

Scratch& threadScratch() {
    thread_local Scratch s;   // ~750 KB per worker, allocated once per thread
    return s;
}

} // namespace

struct DensityGenerator::Impl {
    NoiseNode continental, mountainMask, ridge, hills, entrance;   // 2D
    NoiseNode overhang, caveA, caveB, cheese;                      // 3D

    explicit Impl(const TerrainParams& p) {
#if VXE_WITH_FASTNOISE2
        continental  = NoiseNode(makeFbm(p.continentalScale, 5), 0);
        mountainMask = NoiseNode(makeFbm(p.mountainMaskScale, 3), 101);
        ridge        = NoiseNode(makeRidged(p.ridgeScale, 6), 202);
        hills        = NoiseNode(makeFbm(p.hillScale, 4), 303);
        entrance     = NoiseNode(makeFbm(400.f, 2), 404);
        overhang     = NoiseNode(makeFbm(p.overhangScale, 3), 505);
        caveA        = NoiseNode(makeSimplex(p.spaghettiScale), 606);
        caveB        = NoiseNode(makeSimplex(p.spaghettiScale), 707);
        cheese       = NoiseNode(makeFbm(p.cheeseScale, 3), 808);
#else
        continental  = NoiseNode(Kind::FBm, p.continentalScale, 5, 0);
        mountainMask = NoiseNode(Kind::FBm, p.mountainMaskScale, 3, 101);
        ridge        = NoiseNode(Kind::Ridged, p.ridgeScale, 6, 202);
        hills        = NoiseNode(Kind::FBm, p.hillScale, 4, 303);
        entrance     = NoiseNode(Kind::FBm, 400.f, 2, 404);
        overhang     = NoiseNode(Kind::FBm, p.overhangScale, 3, 505);
        caveA        = NoiseNode(Kind::Simplex, p.spaghettiScale, 1, 606);
        caveB        = NoiseNode(Kind::Simplex, p.spaghettiScale, 1, 707);
        cheese       = NoiseNode(Kind::FBm, p.cheeseScale, 3, 808);
#endif
    }
};

DensityGenerator::DensityGenerator(const TerrainParams& params)
    : m_params(params), m_impl(std::make_unique<Impl>(params)) {}
DensityGenerator::~DensityGenerator() = default;

const char* DensityGenerator::backendName() const {
#if VXE_WITH_FASTNOISE2
    return "FastNoise2 (SIMD)";
#else
    return "software gradient noise";
#endif
}

float DensityGenerator::verticalSlack() const {
    return std::max(m_params.overhangAmpPlains, m_params.overhangAmpMountains) + kDensityClamp;
}

namespace {

struct ColumnShape { float height, mask, entrance; };

ColumnShape shapeColumn(const TerrainParams& p, float cont, float mmask, float ridge, float hills, float entr) {
    ColumnShape c{};
    const float mask = smoothstep(0.05f, 0.45f, mmask) * smoothstep(-0.05f, 0.25f, cont); // no mountains at sea
    const float r01 = saturate(ridge * 0.5f + 0.5f);
    c.height = p.seaLevel + evalSpline(cont) + mask * r01 * r01 * p.ridgeHeight + hills * p.hillHeight * (0.5f + mask);
    c.mask = mask;
    c.entrance = smoothstep(0.35f, 0.6f, entr);
    return c;
}

// Combine column shape + 3D terms into a clamped signed distance and material.
inline void composeSample(const TerrainParams& p, const ColumnShape& col, float wy, float overhang, bool caves,
                          float a, float b, float cheese, float caveDilation, float& outDensity, MaterialId& outMat) {
    float d = wy - col.height;
    d -= overhang * lerp(p.overhangAmpPlains, p.overhangAmpMountains, col.mask);

    bool inCave = false;
    // Caves only carve below the 2D surface. Besides being what we want, this
    // makes the per-chunk "skip caves" early-out exact: a chunk that is
    // entirely above its column heights yields identical samples either way,
    // so shared seam samples always agree with the neighbour.
    if (caves && wy < col.height) {
        // Only the tunnels are widened. Cavern features run to tens of metres,
        // so they survive coarse sampling unaided and widening them would only
        // break the size ordering between the two.
        const float spaghetti = (std::sqrt(a * a + b * b) - p.spaghettiRadius) * p.spaghettiScale * 0.5f - caveDilation;
        const float cavern    = (p.cheeseThreshold - cheese) * p.cheeseScale * 0.25f;
        // Widen before the shell and bedrock guards below, so growing a void can
        // never punch it through the surface or the floor: those two terms still
        // push back at full strength.
        float caveSdf = std::min(spaghetti, cavern);                  // < 0 inside a cave
        const float depth = col.height - wy;
        caveSdf += std::max(0.f, p.caveSurfaceShell - depth) * (1.f - col.entrance);
        caveSdf += std::max(0.f, (p.bedrockY + 24.f) - wy) * 2.f; // solid bedrock floor
        if (-caveSdf > d) { d = -caveSdf; inCave = true; }
    }
    d = std::clamp(d, -kDensityClamp, kDensityClamp);
    outDensity = d;

    if (d >= 0.f) { outMat = kMatAir; return; }
    const float depth = col.height - wy;
    if (inCave || wy < -220.f) outMat = wy < -220.f ? kMatDeepstone : kMatRock;
    else if (depth < 1.5f) {
        if (col.height < p.seaLevel + 2.5f)      outMat = kMatSand;
        else if (col.height > 640.f)             outMat = kMatSnow;
        else if (col.mask > 0.6f)                outMat = kMatRock;
        else                                     outMat = kMatGrass;
    } else if (depth < 10.f) outMat = col.height < p.seaLevel + 2.5f ? kMatGravel : kMatDirt;
    else outMat = kMatRock;
}

} // namespace

ChunkGenStats DensityGenerator::generate(const ChunkKey& key, DensityField& out) const {
    const TerrainParams& p = m_params;
    Scratch& sc = threadScratch();
    ChunkGenStats stats;

    const int32_t stride = key.strideVoxels();
    const float step = float(stride) * world::kVoxelSize;
    const Vec3i o = key.originVoxel();
    // Sample index 0 sits at local point -kFieldOffset.
    const float x0 = float(o.x - kFieldOffset * stride) * world::kVoxelSize;
    const float y0 = float(o.y - kFieldOffset * stride) * world::kVoxelSize;
    const float z0 = float(o.z - kFieldOffset * stride) * world::kVoxelSize;
    const int seed = p.seed;

    // ---- 2D column pass ---------------------------------------------------
    m_impl->continental.grid2D(sc.continental.data(), x0, z0, S, step, seed);
    m_impl->mountainMask.grid2D(sc.mountainMask.data(), x0, z0, S, step, seed);
    m_impl->ridge.grid2D(sc.ridge.data(), x0, z0, S, step, seed);
    m_impl->hills.grid2D(sc.hills.data(), x0, z0, S, step, seed);
    m_impl->entrance.grid2D(sc.entrance.data(), x0, z0, S, step, seed);

    float minH = 1e30f, maxH = -1e30f;
    for (int32_t i = 0; i < kCols; ++i) {
        const ColumnShape c = shapeColumn(p, sc.continental[size_t(i)], sc.mountainMask[size_t(i)], sc.ridge[size_t(i)],
                                          sc.hills[size_t(i)], sc.entrance[size_t(i)]);
        sc.height[size_t(i)] = c.height;
        sc.mask01[size_t(i)] = c.mask;
        sc.entrance01[size_t(i)] = c.entrance;
        minH = std::min(minH, c.height);
        maxH = std::max(maxH, c.height);
    }
    stats.minHeight = minH;
    stats.maxHeight = maxH;

    const float yMin = y0, yMax = y0 + float(S - 1) * step;
    const float slack = verticalSlack();

    // ---- Early-outs -------------------------------------------------------
    if (yMin > maxH + slack) {                         // entirely sky
        out.density.fill(kDensityClamp);
        out.material.fill(kMatAir);
        stats.uniform = true;
        return stats;
    }
    // At LOD >= 2 the chunk is at least a couple of hundred metres away, so only
    // caves near enough to the surface to break through it can be seen; deep
    // ones cost three 3D fields to generate and are behind rock.
    const bool nearSurface = key.lod < 2 || yMax > minH - p.coarseCaveDepth;
    const bool caves = key.lod <= p.maxCaveLod && yMin < maxH && yMax > p.bedrockY && nearSurface;
    // How far a tunnel falls short of being sampleable at this stride, and so
    // how much to widen it by. Zero wherever the tunnels are already wide
    // enough, which includes the whole near field.
    const float tunnelRadius = p.spaghettiRadius * p.spaghettiScale * 0.5f;
    const float wantedRadius = float(stride) * world::kVoxelSize * 0.5f * p.caveDilationMargin;
    const float caveDilation = std::max(0.f, wantedRadius - tunnelRadius);
    const bool overhangBand = yMax > minH - slack && yMin < maxH + slack;

    // ---- 3D pass (only the fields we need) --------------------------------
    if (overhangBand) m_impl->overhang.grid3D(sc.overhang.data(), x0, y0, z0, S, step, seed);
    if (caves) {
        m_impl->caveA.grid3D(sc.caveA.data(), x0, y0, z0, S, step, seed);
        m_impl->caveB.grid3D(sc.caveB.data(), x0, y0, z0, S, step, seed);
        m_impl->cheese.grid3D(sc.cheese.data(), x0, y0, z0, S, step, seed);
    }
    stats.evaluated3D = overhangBand || caves;

    // ---- Compose ----------------------------------------------------------
    for (int32_t z = 0; z < S; ++z) {
        for (int32_t y = 0; y < S; ++y) {
            const float wy = y0 + float(y) * step;
            for (int32_t x = 0; x < S; ++x) {
                const int32_t idx = DensityField::index(x, y, z);
                const int32_t col = x + S * z;
                const ColumnShape cs{sc.height[size_t(col)], sc.mask01[size_t(col)], sc.entrance01[size_t(col)]};
                composeSample(p, cs, wy, overhangBand ? sc.overhang[size_t(idx)] : 0.f, caves,
                              caves ? sc.caveA[size_t(idx)] : 0.f, caves ? sc.caveB[size_t(idx)] : 0.f,
                              caves ? sc.cheese[size_t(idx)] : 0.f, caveDilation, out.density[size_t(idx)],
                              out.material[size_t(idx)]);
            }
        }
    }
    stats.uniform = out.isUniform();
    return stats;
}

float DensityGenerator::sampleHeight(float x, float z) const {
    const int seed = m_params.seed;
    const ColumnShape c = shapeColumn(m_params, m_impl->continental.single2D(x, z, seed),
                                      m_impl->mountainMask.single2D(x, z, seed), m_impl->ridge.single2D(x, z, seed),
                                      m_impl->hills.single2D(x, z, seed), m_impl->entrance.single2D(x, z, seed));
    return c.height;
}

float DensityGenerator::sampleDensity(const Vec3i& v) const {
    const TerrainParams& p = m_params;
    const int seed = p.seed;
    const float x = float(v.x) * world::kVoxelSize, y = float(v.y) * world::kVoxelSize, z = float(v.z) * world::kVoxelSize;
    const ColumnShape c = shapeColumn(p, m_impl->continental.single2D(x, z, seed), m_impl->mountainMask.single2D(x, z, seed),
                                      m_impl->ridge.single2D(x, z, seed), m_impl->hills.single2D(x, z, seed),
                                      m_impl->entrance.single2D(x, z, seed));
    const float slack = verticalSlack();
    if (y > c.height + slack) return kDensityClamp;
    const float oh = std::abs(y - c.height) < slack ? m_impl->overhang.single3D(x, y, z, seed) : 0.f;
    const bool caves = y < c.height && y > p.bedrockY;
    float d; MaterialId m;
    // LOD0 semantics: no dilation, so edits and queries see the true cave.
    composeSample(p, c, y, oh, caves, caves ? m_impl->caveA.single3D(x, y, z, seed) : 0.f,
                  caves ? m_impl->caveB.single3D(x, y, z, seed) : 0.f,
                  caves ? m_impl->cheese.single3D(x, y, z, seed) : 0.f, 0.f, d, m);
    return d;
}

HeightRange DensityGenerator::columnRange(float x0, float z0, float x1, float z1) const {
    // 9x9 probe grid evaluated as SIMD batches (one GenUniformGrid2D per node,
    // ~10x faster than 81 GenSingle2D calls). Ridged peaks between probes are
    // covered by the margin.
    constexpr int kProbe = 9;
    constexpr int kCount = kProbe * kProbe;
    const float step = std::max(x1 - x0, z1 - z0) / float(kProbe - 1);
    const int seed = m_params.seed;
    float cont[kCount], mmask[kCount], ridge[kCount], hills[kCount], entr[kCount];
    m_impl->continental.grid2D(cont, x0, z0, kProbe, step, seed);
    m_impl->mountainMask.grid2D(mmask, x0, z0, kProbe, step, seed);
    m_impl->ridge.grid2D(ridge, x0, z0, kProbe, step, seed);
    m_impl->hills.grid2D(hills, x0, z0, kProbe, step, seed);
    m_impl->entrance.grid2D(entr, x0, z0, kProbe, step, seed);
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < kCount; ++i) {
        const float h = shapeColumn(m_params, cont[i], mmask[i], ridge[i], hills[i], entr[i]).height;
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    }
    const float margin = 24.f + 0.25f * (hi - lo) + verticalSlack();
    return {lo - margin, hi + margin};
}

} // namespace vxe
