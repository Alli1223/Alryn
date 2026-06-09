#pragma once

#include <Alryn/Core/Types.h>

namespace alryn::noise {

// Smooth value noise in roughly [-1, 1].
f32 value2d(f32 x, f32 y, u32 seed = 1337);

// Fractal Brownian motion: sums octaves of value noise. Returns ~[-1, 1].
f32 fbm2d(f32 x, f32 y, int octaves = 4, f32 lacunarity = 2.0f, f32 gain = 0.5f, u32 seed = 1337);

} // namespace alryn::noise
