#include <Alryn/Physics/CollisionWorld.h>

#include <Alryn/Terrain/PropScatter.h>
#include <Alryn/Terrain/TreeScatter.h>
#include <Alryn/World/PropLibrary.h>

#include <algorithm>
#include <cmath>

namespace alryn {

CollisionWorld::CollisionWorld(u32 seed, const PropLibrary& library, f32 chunk_world)
    : seed_(seed), library_(&library), chunk_world_(chunk_world) {}

const std::vector<Collider>& CollisionWorld::chunk_colliders(int cx, int cz) {
    const i64 key = key_of(cx, cz);
    const auto it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second;
    }

    std::vector<Collider> colliders;

    // Trees -> vertical cylinder around the trunk. The trunk is much thinner than the big
    // canopy, so the collider tracks the trunk (and is capped) - you weave between trunks
    // in the dense forest rather than hitting an invisible wall under each canopy.
    for (const TreeInstance& tree : scatter_trees(cx, cz, chunk_world_, seed_)) {
        Collider c;
        c.shape = Collider::Shape::Cylinder;
        c.center = tree.position;
        c.radius = std::min(0.85f, 0.12f * tree.scale);
        c.y_min = tree.position.y - 0.5f;
        c.y_max = tree.position.y + 2.2f * tree.scale; // block at trunk height
        colliders.push_back(c);
    }

    // Solid props (logs, rocks, fences, walls, ...) -> boxes from the prop's local
    // colliders. A prop's local +X stretch (fence rails span the gap to the next post)
    // scales the box's X centre + half-extent so the collider matches the stretched mesh.
    for (const PropInstance& p : scatter_props(cx, cz, chunk_world_, seed_)) {
        const PropDef& def = library_->resolve(p);
        for (const BoxCollider& b : def.colliders) {
            Vec3 center = b.center;
            Vec2 half = b.half_extents;
            center.x *= p.length;
            half.x *= p.length;
            colliders.push_back(
                place_box(center, half, b.height, b.yaw, p.position, p.yaw, p.scale));
        }
    }

    return cache_.emplace(key, std::move(colliders)).first->second;
}

void CollisionWorld::gather(const Vec3& pos, std::vector<Collider>& out) {
    out.clear();
    const int cx = static_cast<int>(std::floor(pos.x / chunk_world_));
    const int cz = static_cast<int>(std::floor(pos.z / chunk_world_));
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const std::vector<Collider>& chunk = chunk_colliders(cx + dx, cz + dz);
            out.insert(out.end(), chunk.begin(), chunk.end());
        }
    }
}

} // namespace alryn
