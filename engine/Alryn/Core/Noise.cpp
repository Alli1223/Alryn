#include <Alryn/Core/Noise.h>

#include <cmath>

namespace alryn::noise {

namespace {

u32 hash(i32 x, i32 y, u32 seed) {
    u32 h = seed + 0x9E3779B9u + static_cast<u32>(x) * 0xC2B2AE35u +
            static_cast<u32>(y) * 0x27D4EB2Fu;
    h = (h ^ (h >> 15)) * 0x85EBCA77u;
    h = (h ^ (h >> 13)) * 0xC2B2AE3Du;
    return h ^ (h >> 16);
}

f32 lattice(i32 x, i32 y, u32 seed) {
    return static_cast<f32>(hash(x, y, seed)) / static_cast<f32>(0xFFFFFFFFu) * 2.0f - 1.0f;
}

f32 smoothstep(f32 t) {
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

f32 value2d(f32 x, f32 y, u32 seed) {
    const f32 xf = std::floor(x);
    const f32 yf = std::floor(y);
    const auto xi = static_cast<i32>(xf);
    const auto yi = static_cast<i32>(yf);
    const f32 tx = smoothstep(x - xf);
    const f32 ty = smoothstep(y - yf);

    const f32 v00 = lattice(xi, yi, seed);
    const f32 v10 = lattice(xi + 1, yi, seed);
    const f32 v01 = lattice(xi, yi + 1, seed);
    const f32 v11 = lattice(xi + 1, yi + 1, seed);

    const f32 a = v00 + (v10 - v00) * tx;
    const f32 b = v01 + (v11 - v01) * tx;
    return a + (b - a) * ty;
}

f32 fbm2d(f32 x, f32 y, int octaves, f32 lacunarity, f32 gain, u32 seed) {
    f32 sum = 0.0f;
    f32 amplitude = 0.5f;
    f32 frequency = 1.0f;
    f32 norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += amplitude * value2d(x * frequency, y * frequency, seed + static_cast<u32>(i) * 131u);
        norm += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

} // namespace alryn::noise
