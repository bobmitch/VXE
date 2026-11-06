// =============================================================================
// Noise.hlsli - periodic 3D gradient noise with analytic derivatives.
//
// All world-space noise is evaluated at (chunk.noiseOrigin + localPos), i.e.
// world position modulo NOISE_PERIOD. Lattices wrap with that period, so the
// wrap is invisible while inputs stay small enough for float precision.
// Frequencies are snapped so NOISE_PERIOD * f is an integer number of cells.
// =============================================================================
#ifndef VXE_NOISE_HLSLI
#define VXE_NOISE_HLSLI

#define NOISE_PERIOD 4096.0

// PCG3D (Jarzynski & Olano 2020)
uint3 pcg3d(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

float3 latticeGradient(int3 cell, int period, uint seed)
{
    int3 c = ((cell % period) + period) % period;
    uint3 h = pcg3d(uint3(c) + seed * uint3(0x9E37u, 0x79B9u, 0x7F4Au));
    return float3(h & 0xFFFFu) * (2.0 / 65535.0) - 1.0;
}

// Returns (value, d/dx, d/dy, d/dz). Value roughly in [-1, 1].
float4 gradientNoiseD(float3 x, int period, uint seed)
{
    int3  i = int3(floor(x));
    float3 w = x - floor(x);
    float3 u  = w * w * w * (w * (w * 6.0 - 15.0) + 10.0);
    float3 du = 30.0 * w * w * (w * (w - 2.0) + 1.0);

    float3 ga = latticeGradient(i + int3(0, 0, 0), period, seed);
    float3 gb = latticeGradient(i + int3(1, 0, 0), period, seed);
    float3 gc = latticeGradient(i + int3(0, 1, 0), period, seed);
    float3 gd = latticeGradient(i + int3(1, 1, 0), period, seed);
    float3 ge = latticeGradient(i + int3(0, 0, 1), period, seed);
    float3 gf = latticeGradient(i + int3(1, 0, 1), period, seed);
    float3 gg = latticeGradient(i + int3(0, 1, 1), period, seed);
    float3 gh = latticeGradient(i + int3(1, 1, 1), period, seed);

    float va = dot(ga, w - float3(0, 0, 0));
    float vb = dot(gb, w - float3(1, 0, 0));
    float vc = dot(gc, w - float3(0, 1, 0));
    float vd = dot(gd, w - float3(1, 1, 0));
    float ve = dot(ge, w - float3(0, 0, 1));
    float vf = dot(gf, w - float3(1, 0, 1));
    float vg = dot(gg, w - float3(0, 1, 1));
    float vh = dot(gh, w - float3(1, 1, 1));

    float value = va + u.x * (vb - va) + u.y * (vc - va) + u.z * (ve - va)
                + u.x * u.y * (va - vb - vc + vd) + u.y * u.z * (va - vc - ve + vg)
                + u.z * u.x * (va - vb - ve + vf) + (-va + vb + vc - vd + ve - vf - vg + vh) * u.x * u.y * u.z;

    float3 deriv = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.z * (ge - ga)
                 + u.x * u.y * (ga - gb - gc + gd) + u.y * u.z * (ga - gc - ge + gg)
                 + u.z * u.x * (ga - gb - ge + gf) + (-ga + gb + gc - gd + ge - gf - gg + gh) * u.x * u.y * u.z
                 + du * (float3(vb, vc, ve) - va
                        + u.yzx * float3(va - vb - vc + vd, va - vc - ve + vg, va - vb - ve + vf)
                        + u.zxy * float3(va - vb - ve + vf, va - vb - vc + vd, va - vc - ve + vg)
                        + u.yzx * u.zxy * (-va + vb + vc - vd + ve - vf - vg + vh));
    return float4(value, deriv);
}

// Snap a frequency so the lattice tiles NOISE_PERIOD exactly.
float periodicFrequency(float f, out int period)
{
    period = max(1, int(round(NOISE_PERIOD * f)));
    return float(period) / NOISE_PERIOD;
}

// fBm with derivatives. p in metres (noise space), baseFreq in cycles/metre.
float4 fbmD(float3 p, float baseFreq, int octaves, uint seed)
{
    float4 sum = 0.0;
    float amp = 0.5;
    float f = baseFreq;
    [loop] for (int o = 0; o < octaves; ++o)
    {
        int period;
        float pf = periodicFrequency(f, period);
        float4 n = gradientNoiseD(p * pf, period, seed + uint(o) * 131u);
        sum += amp * float4(n.x, n.yzw * pf);
        amp *= 0.5;
        f *= 2.0;
    }
    return sum;
}

float fbm(float3 p, float baseFreq, int octaves, uint seed) { return fbmD(p, baseFreq, octaves, seed).x; }

// Cellular F2 - F1 edge distance (thin bright lines at cell borders -> cracks).
float cellularEdge(float3 p, float freq, uint seed)
{
    int period;
    float pf = periodicFrequency(freq, period);
    float3 x = p * pf;
    int3 i = int3(floor(x));
    float3 f = x - floor(x);
    float f1 = 8.0, f2 = 8.0;
    [loop] for (int z = -1; z <= 1; ++z)
    [loop] for (int y = -1; y <= 1; ++y)
    [loop] for (int xx = -1; xx <= 1; ++xx)
    {
        int3 o = int3(xx, y, z);
        int3 c = ((i + o) % period + period) % period;
        float3 jitter = float3(pcg3d(uint3(c) + seed) & 0xFFFFu) / 65535.0;
        float3 d = float3(o) + jitter - f;
        float dist = dot(d, d);
        if (dist < f1) { f2 = f1; f1 = dist; }
        else if (dist < f2) { f2 = dist; }
    }
    return sqrt(f2) - sqrt(f1);
}

// Rock strata: soft sawtooth bands along world Y, warped by low-frequency noise.
float strata(float3 p, float freq)
{
    float w = fbm(p, 0.05, 2, 77u) * 2.0;
    float s = frac(p.y * freq + w);
    return smoothstep(0.0, 0.15, s) * (1.0 - smoothstep(0.85, 1.0, s)) - 0.5;
}

#endif // VXE_NOISE_HLSLI
