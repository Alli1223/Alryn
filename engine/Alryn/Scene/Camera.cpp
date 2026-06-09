#include <Alryn/Scene/Camera.h>

namespace alryn {

void Camera::set_perspective(f32 fov_y_radians, f32 aspect, f32 z_near, f32 z_far) {
    type_ = ProjectionType::Perspective;
    fov_ = fov_y_radians;
    aspect_ = aspect;
    near_ = z_near;
    far_ = z_far;
    rebuild_projection();
}

void Camera::set_orthographic(f32 vertical_size, f32 aspect, f32 z_near, f32 z_far) {
    type_ = ProjectionType::Orthographic;
    ortho_size_ = vertical_size;
    aspect_ = aspect;
    near_ = z_near;
    far_ = z_far;
    rebuild_projection();
}

void Camera::set_aspect(f32 aspect) {
    aspect_ = aspect;
    rebuild_projection();
}

void Camera::look_at(const Vec3& eye, const Vec3& center, const Vec3& up) {
    position_ = eye;
    view_ = alryn::look_at(eye, center, up);
}

void Camera::set_view(const Mat4& view, const Vec3& position) {
    view_ = view;
    position_ = position;
}

void Camera::rebuild_projection() {
    if (type_ == ProjectionType::Perspective) {
        projection_ = alryn::perspective(fov_, aspect_, near_, far_);
    } else {
        const f32 half_h = ortho_size_ * 0.5f;
        const f32 half_w = half_h * aspect_;
        // glm ortho with depth 0..1 (GLM_FORCE_DEPTH_ZERO_TO_ONE), then flip Y
        // to match our Vulkan perspective convention.
        projection_ = glm::ortho(-half_w, half_w, -half_h, half_h, near_, far_);
        projection_[1][1] *= -1.0f;
    }
}

} // namespace alryn
