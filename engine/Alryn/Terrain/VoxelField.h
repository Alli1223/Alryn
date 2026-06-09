#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

#include <functional>
#include <optional>
#include <vector>

namespace alryn {

// A 3D grid of scalar "density" samples defining an isosurface (surface where the
// value crosses the isolevel, default 0). Convention: value < 0 is solid ground,
// value > 0 is air. Values are clamped to [-1, 1] so deformation stays bounded.
//
// `dims` counts corner samples per axis; the number of cells is dims - 1.
class VoxelField {
public:
    VoxelField(const IVec3& dims, f32 voxel_size, const Vec3& origin = Vec3{0.0f});

    const IVec3& dims() const { return dims_; }
    f32 voxel_size() const { return voxel_size_; }
    const Vec3& origin() const { return origin_; }
    IVec3 cell_count() const { return dims_ - IVec3{1}; }

    f32 value(int x, int y, int z) const; // clamps coords into range
    f32 value(const IVec3& c) const { return value(c.x, c.y, c.z); }
    void set(int x, int y, int z, f32 v); // ignores out-of-range; clamps value

    Vec3 world_position(int x, int y, int z) const {
        return origin_ + Vec3{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)} *
                             voxel_size_;
    }
    Vec3 world_position(const IVec3& c) const { return world_position(c.x, c.y, c.z); }

    // Sets every sample from a world-space density function.
    void fill(const std::function<f32(const Vec3&)>& density);

    // Adds `amount * falloff` to samples within `radius` of `center`. Positive
    // amount carves toward air; negative adds solid. Returns the affected voxel
    // box (for marking chunks dirty).
    struct Region {
        IVec3 min{0};
        IVec3 max{0};
        bool valid = false;
    };
    Region apply_sphere(const Vec3& center, f32 radius, f32 amount);

    // Trilinearly samples the field at a world position.
    f32 sample(const Vec3& world_pos) const;

    // Marches a ray until it crosses from air into solid; returns the world hit.
    std::optional<Vec3> raycast(const Vec3& origin, const Vec3& dir, f32 max_dist,
                                f32 iso = 0.0f) const;

private:
    bool in_range(int x, int y, int z) const;
    usize index(int x, int y, int z) const;

    IVec3 dims_;
    f32 voxel_size_;
    Vec3 origin_;
    std::vector<f32> data_;
};

} // namespace alryn
