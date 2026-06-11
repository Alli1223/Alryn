#include <Alryn/Physics/CollisionWorld.h>

#include <Alryn/Terrain/PropScatter.h>
#include <Alryn/Terrain/TreeScatter.h>
#include <Alryn/World/PropLibrary.h>

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

    // Trees -> vertical cylinder around the trunk.
    for (const TreeInstance& tree : scatter_trees(cx, cz, chunk_world_, seed_)) {
        Collider c;
        c.shape = Collider::Shape::Cylinder;
        c.center = tree.position;
        c.radius = 0.28f * tree.scale;
        c.y_min = tree.position.y - 0.5f;
        c.y_max = tree.position.y + 2.2f * tree.scale; // block at trunk height
        colliders.push_back(c);
    }

    // Solid forest props (fallen logs) -> boxes from the prop's local colliders.
    for (const PropInstance& p : scatter_props(cx, cz, chunk_world_, seed_)) {
        const PropDef& def = library_->resolve(p);
        for (const BoxCollider& b : def.colliders) {
            colliders.push_back(
                place_box(b.center, b.half_extents, b.height, b.yaw, p.position, p.yaw, p.scale));
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
