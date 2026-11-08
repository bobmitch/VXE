#include "vxe/world/DensityGenerator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
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
    std::vector<float> gradA, gradB, gradC, gradD, heightDx, heightDz;   // stripe erosion only
    std::vector<float> overhang, caveSdf;
    Scratch()
        : continental(kCols), mountainMask(kCols), ridge(kCols), hills(kCols), entrance(kCols),
          height(kCols), mask01(kCols), entrance01(kCols),
          gradA(kCols), gradB(kCols), gradC(kCols), gradD(kCols), heightDx(kCols), heightDz(kCols),
          overhang(size_t(kFieldVolume)), caveSdf(size_t(kFieldVolume)) {}
};

Scratch& threadScratch() {
    thread_local Scratch s;   // ~750 KB per worker, allocated once per thread
    return s;
}

} // namespace

struct DensityGenerator::Impl {
    NoiseNode continental, mountainMask, ridge, hills, entrance;   // 2D
    NoiseNode overhang;                                            // 3D

    explicit Impl(const TerrainParams& p) {
#if VXE_WITH_FASTNOISE2
        continental  = NoiseNode(makeFbm(p.continentalScale, 5), 0);
        mountainMask = NoiseNode(makeFbm(p.mountainMaskScale, 3), 101);
        ridge        = NoiseNode(makeRidged(p.ridgeScale, 6), 202);
        hills        = NoiseNode(makeFbm(p.hillScale, 4), 303);
        entrance     = NoiseNode(makeFbm(400.f, 2), 404);
        overhang     = NoiseNode(makeFbm(p.overhangScale, 3), 505);
#else
        continental  = NoiseNode(Kind::FBm, p.continentalScale, 5, 0);
        mountainMask = NoiseNode(Kind::FBm, p.mountainMaskScale, 3, 101);
        ridge        = NoiseNode(Kind::Ridged, p.ridgeScale, 6, 202);
        hills        = NoiseNode(Kind::FBm, p.hillScale, 4, 303);
        entrance     = NoiseNode(Kind::FBm, 400.f, 2, 404);
        overhang     = NoiseNode(Kind::FBm, p.overhangScale, 3, 505);
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
                          float caveField, float& outDensity, MaterialId& outMat) {
    float d = wy - col.height;
    d -= overhang * lerp(p.overhangAmpPlains, p.overhangAmpMountains, col.mask);

    bool inCave = false;
    // No surface shell any more. The old noise caves needed one because they
    // surfaced at random; a placed network decides where it breaks daylight,
    // and the throat at each entrance is meant to.
    if (caves) {
        float caveSdf = caveField;      // already widened for this LOD by the splat
        caveSdf += std::max(0.f, (p.bedrockY + 24.f) - wy) * 2.f;   // solid bedrock floor
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

    for (int32_t i = 0; i < kCols; ++i) {
        const ColumnShape c = shapeColumn(p, sc.continental[size_t(i)], sc.mountainMask[size_t(i)], sc.ridge[size_t(i)],
                                          sc.hills[size_t(i)], sc.entrance[size_t(i)]);
        sc.height[size_t(i)] = c.height;
        sc.mask01[size_t(i)] = c.mask;
        sc.entrance01[size_t(i)] = c.entrance;
    }

    // ---- Stripe erosion ---------------------------------------------------
    // The filter needs the gradient of the unfiltered height. Taking it from
    // this chunk's own grid would measure it over the sample spacing -- 1 m at
    // LOD0, 8 m at LOD3 -- and the gullies would then be a different shape at
    // every level and change under you at each transition. Two extra grids at a
    // fixed epsilon make it a pure function of world position instead.
    if (p.stripeErosion) {
        const float kGradEps = p.stripe.gradEps;
        const auto heightsAt = [&](float ox, float oz, std::vector<float>& out) {
            m_impl->continental.grid2D(sc.gradA.data(), ox, oz, S, step, seed);
            m_impl->mountainMask.grid2D(sc.gradB.data(), ox, oz, S, step, seed);
            m_impl->ridge.grid2D(sc.gradC.data(), ox, oz, S, step, seed);
            m_impl->hills.grid2D(sc.gradD.data(), ox, oz, S, step, seed);
            for (int32_t i = 0; i < kCols; ++i)   // `entrance` does not reach height
                out[size_t(i)] = shapeColumn(p, sc.gradA[size_t(i)], sc.gradB[size_t(i)], sc.gradC[size_t(i)],
                                             sc.gradD[size_t(i)], 0.f).height;
        };
        heightsAt(x0 + kGradEps, z0, sc.heightDx);
        heightsAt(x0, z0 + kGradEps, sc.heightDz);

        for (int32_t zi = 0; zi < S; ++zi)
            for (int32_t xi = 0; xi < S; ++xi) {
                const size_t i = size_t(xi + S * zi);
                const float h = sc.height[i];
                SlopeXZ g{(sc.heightDx[i] - h) * (1.f / kGradEps), (sc.heightDz[i] - h) * (1.f / kGradEps)};
                sc.height[i] = h + stripeErosion(p.stripe, x0 + float(xi) * step, z0 + float(zi) * step, g);
            }
    }

    float minH = 1e30f, maxH = -1e30f;
    for (int32_t i = 0; i < kCols; ++i) {
        minH = std::min(minH, sc.height[size_t(i)]);
        maxH = std::max(maxH, sc.height[size_t(i)]);
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
    const bool caves = p.caves && key.lod <= p.maxCaveLod && yMin < maxH && yMax > p.bedrockY && nearSurface;
    // How far a passage falls short of being sampleable at this stride, and so
    // how much to widen it by. Zero wherever passages are already wide enough,
    // which includes the whole near field. A placed capsule can be widened
    // exactly; the noise fields this replaces could only be thresholded.
    const float wantedRadius = float(stride) * world::kVoxelSize * 0.5f * p.caveDilationMargin;
    const float caveGrow = std::max(0.f, wantedRadius - p.cave.tunnelRadius);
    const bool overhangBand = yMax > minH - slack && yMin < maxH + slack;

    // ---- 3D pass (only the fields we need) --------------------------------
    if (overhangBand) m_impl->overhang.grid3D(sc.overhang.data(), x0, y0, z0, S, step, seed);
    // Caves are splatted, not sampled: every passage near this chunk writes its
    // own distance into the field and nothing else is touched, so a chunk with
    // no cave in it pays for one bounds test per system rather than 38^3
    // evaluations of anything.
    bool anyCave = false;
    if (caves) {
        const Vec3f origin{x0, y0, z0};
        const float span = float(S - 1) * step;
        const Aabbf chunkBounds{origin, origin + Vec3f(span)};
        // Widened by the same slack a system is allowed to stray outside its
        // own cell, so a passage reaching in from next door is still asked for.
        const float reach = kDensityClamp + caveGrow + p.cave.regionSize * kCellSlack;
        const int32_t c0x = int32_t(std::floor((x0 - reach) / p.cave.regionSize));
        const int32_t c1x = int32_t(std::floor((x0 + span + reach) / p.cave.regionSize));
        const int32_t c0z = int32_t(std::floor((z0 - reach) / p.cave.regionSize));
        const int32_t c1z = int32_t(std::floor((z0 + span + reach) / p.cave.regionSize));
        for (int32_t cz = c0z; cz <= c1z; ++cz)
            for (int32_t cx = c0x; cx <= c1x; ++cx) {
                const CaveSystem& sysRef = cachedSystem(cx, cz);
                if (sysRef.empty()) continue;
                const Aabbf grown{sysRef.bounds.min - Vec3f(reach), sysRef.bounds.max + Vec3f(reach)};
                if (!grown.intersects(chunkBounds)) continue;
                if (!anyCave) { std::fill(sc.caveSdf.begin(), sc.caveSdf.end(), 1e30f); anyCave = true; }
                splatCaves(sysRef, origin, step, S, caveGrow, kDensityClamp, sc.caveSdf.data());
            }
    }
    stats.evaluated3D = overhangBand || anyCave;

    // ---- Compose ----------------------------------------------------------
    for (int32_t z = 0; z < S; ++z) {
        for (int32_t y = 0; y < S; ++y) {
            const float wy = y0 + float(y) * step;
            for (int32_t x = 0; x < S; ++x) {
                const int32_t idx = DensityField::index(x, y, z);
                const int32_t col = x + S * z;
                const ColumnShape cs{sc.height[size_t(col)], sc.mask01[size_t(col)], sc.entrance01[size_t(col)]};
                composeSample(p, cs, wy, overhangBand ? sc.overhang[size_t(idx)] : 0.f, anyCave,
                              anyCave ? sc.caveSdf[size_t(idx)] : 1e30f, out.density[size_t(idx)],
                              out.material[size_t(idx)]);
            }
        }
    }
    stats.uniform = out.isUniform();
    return stats;
}

SlopeXZ DensityGenerator::baseGradient(float x, float z) const {
    const float kGradEps = m_params.stripe.gradEps;   // must match the grid path, or
    const int seed = m_params.seed;                   // edits and meshes would disagree
    const auto h = [&](float px, float pz) {
        return shapeColumn(m_params, m_impl->continental.single2D(px, pz, seed),
                           m_impl->mountainMask.single2D(px, pz, seed), m_impl->ridge.single2D(px, pz, seed),
                           m_impl->hills.single2D(px, pz, seed), 0.f).height;
    };
    const float h0 = h(x, z);
    return {(h(x + kGradEps, z) - h0) * (1.f / kGradEps), (h(x, z + kGradEps) - h0) * (1.f / kGradEps)};
}

// Systems are generated per region cell and cached per thread: a chunk build
// touches a handful of cells, and the probing that finds the entrances costs
// more than the walk that follows it. Thread-local rather than shared so the
// workers never contend; the duplicated work across threads is small against a
// lock on every sample.
const CaveSystem& DensityGenerator::cachedSystem(int32_t cellX, int32_t cellZ) const {
    struct Cache {
        std::unordered_map<uint64_t, CaveSystem> map;
    };
    thread_local Cache cache;
    const uint64_t key = (uint64_t(uint32_t(cellX)) << 32) | uint32_t(cellZ);
    auto it = cache.map.find(key);
    if (it != cache.map.end()) return it->second;
    if (cache.map.size() > 512) cache.map.clear();     // bounded; cheap to rebuild

    // Entrances are placed against the surface this generator actually makes,
    // erosion included, or a cave mouth would sit in the wrong hollow.
    const HeightFn h = [this](float wx, float wz) { return sampleHeight(wx, wz); };
    CaveSystem sys = buildCaveSystem(m_params.cave, m_params.seed, cellX, cellZ, h);
    return cache.map.emplace(key, std::move(sys)).first->second;
}

float DensityGenerator::sampleHeight(float x, float z) const {
    const int seed = m_params.seed;
    const ColumnShape c = shapeColumn(m_params, m_impl->continental.single2D(x, z, seed),
                                      m_impl->mountainMask.single2D(x, z, seed), m_impl->ridge.single2D(x, z, seed),
                                      m_impl->hills.single2D(x, z, seed), m_impl->entrance.single2D(x, z, seed));
    if (!m_params.stripeErosion) return c.height;
    SlopeXZ g = baseGradient(x, z);
    return c.height + stripeErosion(m_params.stripe, x, z, g);
}

float DensityGenerator::sampleDensity(const Vec3i& v) const {
    const TerrainParams& p = m_params;
    const int seed = p.seed;
    const float x = float(v.x) * world::kVoxelSize, y = float(v.y) * world::kVoxelSize, z = float(v.z) * world::kVoxelSize;
    ColumnShape c = shapeColumn(p, m_impl->continental.single2D(x, z, seed), m_impl->mountainMask.single2D(x, z, seed),
                                m_impl->ridge.single2D(x, z, seed), m_impl->hills.single2D(x, z, seed),
                                m_impl->entrance.single2D(x, z, seed));
    if (p.stripeErosion) {
        SlopeXZ g = baseGradient(x, z);
        c.height += stripeErosion(p.stripe, x, z, g);
    }
    const float slack = verticalSlack();
    if (y > c.height + slack) return kDensityClamp;
    const float oh = std::abs(y - c.height) < slack ? m_impl->overhang.single3D(x, y, z, seed) : 0.f;
    // LOD0 semantics: no widening, so edits and queries see the true passage.
    float caveSdf = 1e30f;
    if (p.caves && y > p.bedrockY) {
        const int32_t cx = int32_t(std::floor(x / p.cave.regionSize));
        const int32_t cz = int32_t(std::floor(z / p.cave.regionSize));
        const int32_t r = 1 + int32_t(kCellSlack);     // matches the chunk path's widening
        for (int32_t dz = -r; dz <= r; ++dz)
            for (int32_t dx = -r; dx <= r; ++dx) {
                const CaveSystem& sysRef = cachedSystem(cx + dx, cz + dz);
                if (!sysRef.empty()) caveSdf = std::min(caveSdf, caveDistance(sysRef, {x, y, z}, 0.f));
            }
    }
    float d; MaterialId m;
    composeSample(p, c, y, oh, caveSdf < 1e29f, caveSdf, d, m);
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
    // The probes above are unfiltered, so the envelope has to allow for however
    // far the erosion could move a column either way.
    const float stripeSlack = m_params.stripeErosion ? stripeMaxOffset(m_params.stripe) : 0.f;
    const float margin = 24.f + 0.25f * (hi - lo) + verticalSlack() + stripeSlack;
    return {lo - margin, hi + margin};
}

} // namespace vxe
