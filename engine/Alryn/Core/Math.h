#pragma once

#include <Alryn/Core/Types.h>

// GLM is configured for Vulkan project-wide (see engine/CMakeLists.txt). The
// guards below keep things correct even when this header is used standalone.
#ifndef GLM_FORCE_RADIANS
    #define GLM_FORCE_RADIANS
#endif
#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
    #define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif
#ifndef GLM_ENABLE_EXPERIMENTAL
    #define GLM_ENABLE_EXPERIMENTAL
#endif

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <numbers>

namespace alryn {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using IVec2 = glm::ivec2;
using IVec3 = glm::ivec3;
using UVec2 = glm::uvec2;
using UVec3 = glm::uvec3;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using Quat = glm::quat;

inline constexpr f32 Pi = std::numbers::pi_v<f32>;
inline constexpr f32 TwoPi = 2.0f * Pi;
inline constexpr f32 HalfPi = Pi * 0.5f;

constexpr Quat QuatIdentity{1.0f, 0.0f, 0.0f, 0.0f}; // (w, x, y, z)

inline f32 radians(f32 degrees) { return glm::radians(degrees); }
inline f32 degrees(f32 radians) { return glm::degrees(radians); }

// Vulkan-correct perspective projection: depth maps to [0, 1] (via
// GLM_FORCE_DEPTH_ZERO_TO_ONE) and we flip Y so +Y is up in NDC.
inline Mat4 perspective(f32 fovy_radians, f32 aspect, f32 z_near, f32 z_far) {
    Mat4 proj = glm::perspective(fovy_radians, aspect, z_near, z_far);
    proj[1][1] *= -1.0f;
    return proj;
}

inline Mat4 look_at(const Vec3& eye, const Vec3& center, const Vec3& up = Vec3{0.0f, 1.0f, 0.0f}) {
    return glm::lookAt(eye, center, up);
}

} // namespace alryn
