#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

#include <functional>
#include <optional>

namespace alryn {

// A density field as a pure function of world position: < 0 solid, > 0 air. This
// is the abstraction that lets the world be infinite/streamed - the server and
// clients sample it anywhere instead of owning one bounded VoxelField.
using DensitySampler = std::function<f32(const Vec3&)>;

// Marches a ray through a DensitySampler until it crosses from air into solid;
// returns the world-space hit (used for ground-finding, spawn, and dig aiming).
std::optional<Vec3> raycast_density(const DensitySampler& density, const Vec3& origin,
                                    const Vec3& dir, f32 max_dist, f32 iso = 0.0f,
                                    f32 step = 0.25f);

} // namespace alryn
