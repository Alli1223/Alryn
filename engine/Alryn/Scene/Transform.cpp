#include <Alryn/Scene/Transform.h>

namespace alryn {

const Mat4& Transform::matrix() const {
    if (dirty_) {
        const Mat4 t = glm::translate(Mat4{1.0f}, position_);
        const Mat4 r = glm::mat4_cast(rotation_);
        const Mat4 s = glm::scale(Mat4{1.0f}, scale_);
        cached_ = t * r * s;
        dirty_ = false;
    }
    return cached_;
}

} // namespace alryn
