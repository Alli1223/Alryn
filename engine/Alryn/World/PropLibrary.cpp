#include <Alryn/World/PropLibrary.h>

#include <Alryn/Renderer/MeshPrimitives.h>

namespace alryn {

PropDef PropLibrary::build_bush(int variant) {
    PropDef def;
    def.name = "bush";
    static const Vec3 greens[] = {{0.22f, 0.42f, 0.20f}, {0.26f, 0.46f, 0.22f}, {0.30f, 0.40f, 0.18f}};
    def.parts.push_back({primitives::bush(variant, greens[variant % 3]), PropLayer::Foliage});
    return def;
}

PropDef PropLibrary::build_rock(int variant) {
    PropDef def;
    def.name = "rock";
    def.parts.push_back({primitives::rock(variant), PropLayer::Opaque});
    return def;
}

PropDef PropLibrary::build_log(int variant) {
    PropDef def;
    def.name = "log";
    const Vec3 bark{0.34f, 0.26f, 0.18f};
    def.parts.push_back({primitives::fallen_log(variant, bark), PropLayer::Opaque});
    // The log lies along local +X; a box over its xz extent blocks the player. It
    // rotates with the prop's yaw, so it stays aligned with the visible log.
    BoxCollider c;
    c.center = Vec3{0.0f};
    c.half_extents = Vec2{0.85f, 0.26f};
    c.height = 0.55f;
    def.colliders.push_back(c);
    return def;
}

PropLibrary::PropLibrary() {
    for (int i = 0; i < 3; ++i) {
        bushes_.push_back(build_bush(i));
    }
    for (int i = 0; i < 3; ++i) {
        rocks_.push_back(build_rock(i));
    }
    for (int i = 0; i < 3; ++i) {
        logs_.push_back(build_log(i));
    }
}

const PropDef& PropLibrary::resolve(const PropInstance& inst) const {
    switch (inst.category) {
        case PropCategory::Bush: return bushes_[inst.variant % bushes_.size()];
        case PropCategory::Rock: return rocks_[inst.variant % rocks_.size()];
        case PropCategory::Log: return logs_[inst.variant % logs_.size()];
    }
    return bushes_[0];
}

} // namespace alryn
