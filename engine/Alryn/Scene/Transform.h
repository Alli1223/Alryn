#pragma once

#include <Alryn/Core/Math.h>

namespace alryn {

// Position / rotation / scale with a lazily-rebuilt local TRS matrix.
class Transform {
public:
    Transform() = default;
    explicit Transform(const Vec3& position, const Quat& rotation = QuatIdentity,
                       const Vec3& scale = Vec3{1.0f})
        : position_(position), rotation_(rotation), scale_(scale) {}

    const Vec3& position() const { return position_; }
    const Quat& rotation() const { return rotation_; }
    const Vec3& scale() const { return scale_; }

    void set_position(const Vec3& position) {
        position_ = position;
        dirty_ = true;
    }
    void set_rotation(const Quat& rotation) {
        rotation_ = rotation;
        dirty_ = true;
    }
    void set_scale(const Vec3& scale) {
        scale_ = scale;
        dirty_ = true;
    }
    // Euler angles in radians (pitch=x, yaw=y, roll=z).
    void set_euler(const Vec3& euler_radians) {
        rotation_ = Quat{euler_radians};
        dirty_ = true;
    }

    void translate(const Vec3& delta) {
        position_ += delta;
        dirty_ = true;
    }
    void rotate(const Quat& delta) {
        rotation_ = glm::normalize(delta * rotation_);
        dirty_ = true;
    }

    Vec3 forward() const { return rotation_ * Vec3{0.0f, 0.0f, -1.0f}; }
    Vec3 right() const { return rotation_ * Vec3{1.0f, 0.0f, 0.0f}; }
    Vec3 up() const { return rotation_ * Vec3{0.0f, 1.0f, 0.0f}; }

    // Local transform matrix (translate * rotate * scale), cached.
    const Mat4& matrix() const;

private:
    Vec3 position_{0.0f};
    Quat rotation_{QuatIdentity};
    Vec3 scale_{1.0f};

    mutable Mat4 cached_{1.0f};
    mutable bool dirty_ = true;
};

} // namespace alryn
