#include <Alryn/Terrain/VoxelField.h>

#include <algorithm>
#include <cmath>

namespace alryn {

VoxelField::VoxelField(const IVec3& dims, f32 voxel_size, const Vec3& origin)
    : dims_(dims), voxel_size_(voxel_size), origin_(origin),
      data_(static_cast<usize>(dims.x) * static_cast<usize>(dims.y) * static_cast<usize>(dims.z),
            1.0f) {} // start as all air

bool VoxelField::in_range(int x, int y, int z) const {
    return x >= 0 && y >= 0 && z >= 0 && x < dims_.x && y < dims_.y && z < dims_.z;
}

usize VoxelField::index(int x, int y, int z) const {
    return static_cast<usize>(x) +
           static_cast<usize>(dims_.x) *
               (static_cast<usize>(y) + static_cast<usize>(dims_.y) * static_cast<usize>(z));
}

f32 VoxelField::value(int x, int y, int z) const {
    x = std::clamp(x, 0, dims_.x - 1);
    y = std::clamp(y, 0, dims_.y - 1);
    z = std::clamp(z, 0, dims_.z - 1);
    return data_[index(x, y, z)];
}

void VoxelField::set(int x, int y, int z, f32 v) {
    if (in_range(x, y, z)) {
        data_[index(x, y, z)] = std::clamp(v, -1.0f, 1.0f);
    }
}

void VoxelField::fill(const std::function<f32(const Vec3&)>& density) {
    for (int z = 0; z < dims_.z; ++z) {
        for (int y = 0; y < dims_.y; ++y) {
            for (int x = 0; x < dims_.x; ++x) {
                data_[index(x, y, z)] = std::clamp(density(world_position(x, y, z)), -1.0f, 1.0f);
            }
        }
    }
}

VoxelField::Region VoxelField::apply_sphere(const Vec3& center, f32 radius, f32 amount) {
    Region region;
    const Vec3 lo = (center - Vec3{radius} - origin_) / voxel_size_;
    const Vec3 hi = (center + Vec3{radius} - origin_) / voxel_size_;
    IVec3 mn{static_cast<int>(std::floor(lo.x)), static_cast<int>(std::floor(lo.y)),
             static_cast<int>(std::floor(lo.z))};
    IVec3 mx{static_cast<int>(std::ceil(hi.x)), static_cast<int>(std::ceil(hi.y)),
             static_cast<int>(std::ceil(hi.z))};
    mn = glm::max(mn, IVec3{0});
    mx = glm::min(mx, dims_ - IVec3{1});

    for (int z = mn.z; z <= mx.z; ++z) {
        for (int y = mn.y; y <= mx.y; ++y) {
            for (int x = mn.x; x <= mx.x; ++x) {
                const Vec3 p = world_position(x, y, z);
                const f32 d = glm::length(p - center);
                if (d <= radius) {
                    const f32 falloff = 1.0f - d / radius;
                    set(x, y, z, value(x, y, z) + amount * falloff);
                    const IVec3 here{x, y, z};
                    if (!region.valid) {
                        region.min = here;
                        region.max = here;
                        region.valid = true;
                    } else {
                        region.min = glm::min(region.min, here);
                        region.max = glm::max(region.max, here);
                    }
                }
            }
        }
    }
    return region;
}

f32 VoxelField::sample(const Vec3& world_pos) const {
    const Vec3 vc = (world_pos - origin_) / voxel_size_;
    const f32 xf = std::floor(vc.x);
    const f32 yf = std::floor(vc.y);
    const f32 zf = std::floor(vc.z);
    const auto x = static_cast<int>(xf);
    const auto y = static_cast<int>(yf);
    const auto z = static_cast<int>(zf);
    const f32 tx = vc.x - xf;
    const f32 ty = vc.y - yf;
    const f32 tz = vc.z - zf;

    const f32 c000 = value(x, y, z);
    const f32 c100 = value(x + 1, y, z);
    const f32 c010 = value(x, y + 1, z);
    const f32 c110 = value(x + 1, y + 1, z);
    const f32 c001 = value(x, y, z + 1);
    const f32 c101 = value(x + 1, y, z + 1);
    const f32 c011 = value(x, y + 1, z + 1);
    const f32 c111 = value(x + 1, y + 1, z + 1);

    const f32 x00 = c000 + (c100 - c000) * tx;
    const f32 x10 = c010 + (c110 - c010) * tx;
    const f32 x01 = c001 + (c101 - c001) * tx;
    const f32 x11 = c011 + (c111 - c011) * tx;
    const f32 y0 = x00 + (x10 - x00) * ty;
    const f32 y1 = x01 + (x11 - x01) * ty;
    return y0 + (y1 - y0) * tz;
}

std::optional<Vec3> VoxelField::raycast(const Vec3& origin, const Vec3& dir, f32 max_dist,
                                        f32 iso) const {
    const Vec3 d = glm::normalize(dir);
    const f32 step = voxel_size_ * 0.5f;
    f32 prev = sample(origin);
    for (f32 t = step; t <= max_dist; t += step) {
        const Vec3 p = origin + d * t;
        const f32 v = sample(p);
        if (prev > iso && v <= iso) { // crossed from air into solid
            const f32 denom = v - prev;
            const f32 frac = denom != 0.0f ? (iso - prev) / denom : 0.5f;
            return origin + d * (t - step) + d * step * std::clamp(frac, 0.0f, 1.0f);
        }
        prev = v;
    }
    return std::nullopt;
}

} // namespace alryn
