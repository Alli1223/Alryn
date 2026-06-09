#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Noise.h>
#include <Alryn/Core/Types.h>

// Deterministic world description shared by the server and every client, so they
// all generate identical terrain from a seed (only runtime deformations are
// replicated over the network).
namespace alryn::worldgen {

inline const IVec3 dims{81, 49, 81};
inline constexpr f32 voxel_size = 0.5f;
inline const Vec3 origin{-20.0f, -12.0f, -20.0f};

inline f32 density(const Vec3& p, u32 seed) {
    const f32 s = static_cast<f32>(seed % 997u) * 0.137f;
    const f32 height = 6.0f * noise::fbm2d(p.x * 0.05f + s, p.z * 0.05f + s, 4) +
                       1.5f * noise::fbm2d(p.x * 0.18f, p.z * 0.18f, 3);
    return p.y - height; // solid below the rolling height field
}

} // namespace alryn::worldgen
