#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Physics/Collider.h>

#include <unordered_map>
#include <vector>

namespace alryn {

class PropLibrary;

// The static-collision side of the lightweight physics layer. Generates tree
// (cylinder) and house-wall (box) colliders deterministically from the world seed,
// chunk by chunk, and caches them. The character controller (and, later,
// projectiles) query `gather()` for the colliders near a point. This is the seam a
// real physics library (Jolt) would sit behind.
class CollisionWorld {
public:
    CollisionWorld(u32 seed, const PropLibrary& library, f32 chunk_world = 8.0f);

    // Appends colliders from the 3x3 chunks around `pos` into `out` (cleared first).
    void gather(const Vec3& pos, std::vector<Collider>& out);

    usize cached_chunks() const { return cache_.size(); }

private:
    const std::vector<Collider>& chunk_colliders(int cx, int cz);
    static i64 key_of(int cx, int cz) {
        return (static_cast<i64>(cx) << 32) | static_cast<i64>(static_cast<u32>(cz));
    }

    u32 seed_;
    const PropLibrary* library_;
    f32 chunk_world_;
    std::unordered_map<i64, std::vector<Collider>> cache_;
};

} // namespace alryn
