#include <Alryn/Core/Density.h>

namespace alryn {

std::optional<Vec3> raycast_density(const DensitySampler& density, const Vec3& origin,
                                    const Vec3& dir, f32 max_dist, f32 iso, f32 step) {
    const Vec3 d = glm::normalize(dir);
    f32 prev = density(origin);
    for (f32 t = step; t <= max_dist; t += step) {
        const Vec3 p = origin + d * t;
        const f32 v = density(p);
        if (prev > iso && v <= iso) { // crossed air -> solid
            const f32 denom = v - prev;
            const f32 frac = denom != 0.0f ? (iso - prev) / denom : 0.5f;
            return origin + d * (t - step) + d * step * glm::clamp(frac, 0.0f, 1.0f);
        }
        prev = v;
    }
    return std::nullopt;
}

} // namespace alryn
