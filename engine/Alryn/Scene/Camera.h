#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

namespace alryn {

enum class ProjectionType { Perspective, Orthographic };

// View + projection provider. Projection is Vulkan-correct (depth 0..1, Y down)
// via alryn::perspective. Keep this engine-side and feed view_projection() to the
// renderer.
class Camera {
public:
    void set_perspective(f32 fov_y_radians, f32 aspect, f32 z_near, f32 z_far);
    void set_orthographic(f32 vertical_size, f32 aspect, f32 z_near, f32 z_far);
    void set_aspect(f32 aspect);

    void look_at(const Vec3& eye, const Vec3& center, const Vec3& up = Vec3{0.0f, 1.0f, 0.0f});
    void set_view(const Mat4& view, const Vec3& position);

    const Mat4& view() const { return view_; }
    const Mat4& projection() const { return projection_; }
    Mat4 view_projection() const { return projection_ * view_; }

    const Vec3& position() const { return position_; }
    ProjectionType projection_type() const { return type_; }

private:
    void rebuild_projection();

    ProjectionType type_ = ProjectionType::Perspective;
    f32 fov_ = radians(60.0f);
    f32 aspect_ = 16.0f / 9.0f;
    f32 near_ = 0.1f;
    f32 far_ = 1000.0f;
    f32 ortho_size_ = 10.0f;

    Mat4 projection_{1.0f};
    Mat4 view_{1.0f};
    Vec3 position_{0.0f};
};

} // namespace alryn
