#include <doctest/doctest.h>

#include <Alryn/Core/Density.h>
#include <Alryn/Physics/CharacterController.h>
#include <Alryn/Physics/Collider.h>
#include <Alryn/Physics/CollisionWorld.h>
#include <Alryn/Physics/Projectile.h>
#include <Alryn/Platform/Events.h>
#include <Alryn/Platform/Input.h>
#include <Alryn/World/PropLibrary.h>

#include <algorithm>
#include <array>
#include <vector>

using namespace alryn;

TEST_CASE("Input: tracks keys, buttons, and per-frame mouse delta") {
    Input input;

    KeyPressedEvent press{65};
    input.on_event(press);
    CHECK(input.key_down(65));
    KeyReleasedEvent release{65};
    input.on_event(release);
    CHECK_FALSE(input.key_down(65));

    MouseButtonPressedEvent click{0};
    input.on_event(click);
    CHECK(input.mouse_down(0));

    MouseMovedEvent first{100.0f, 100.0f};
    input.on_event(first); // first event only sets the baseline
    input.on_update(Timestep{0.016f});
    CHECK(input.mouse_delta().x == doctest::Approx(0.0f));

    MouseMovedEvent moved{110.0f, 105.0f};
    input.on_event(moved);
    input.on_update(Timestep{0.016f});
    CHECK(input.mouse_delta().x == doctest::Approx(10.0f));
    CHECK(input.mouse_delta().y == doctest::Approx(5.0f));
}

namespace {
// A flat solid floor at y = floor_y, as a density function.
DensitySampler flat_floor(f32 floor_y = 2.0f) {
    return [floor_y](const Vec3& p) { return std::clamp(p.y - floor_y, -1.0f, 1.0f); };
}
constexpr Timestep kStep{1.0f / 60.0f};
} // namespace

TEST_CASE("CharacterController: falls under gravity and lands on the floor") {
    const DensitySampler density = flat_floor(2.0f);
    CharacterController character;
    character.set_position(Vec3{0.0f, 8.0f, 0.0f});

    for (int i = 0; i < 300; ++i) {
        character.update(density, Vec3{0.0f}, false, kStep);
    }
    CHECK(character.on_ground());
    CHECK(character.position().y == doctest::Approx(2.0f).epsilon(0.15));
}

TEST_CASE("CharacterController: walks across flat ground while staying grounded") {
    const DensitySampler density = flat_floor(2.0f);
    CharacterController character;
    character.set_position(Vec3{0.0f, 2.0f, 0.0f});

    for (int i = 0; i < 120; ++i) {
        character.update(density, Vec3{1.0f, 0.0f, 0.0f}, false, kStep);
    }
    CHECK(character.position().x > 2.0f);
    CHECK(character.position().y == doctest::Approx(2.0f).epsilon(0.2));
    CHECK(character.on_ground());
}

TEST_CASE("CharacterController: a wall blocks horizontal movement") {
    // Solid everywhere past x = 5, floor at y = 2 otherwise.
    const DensitySampler density = [](const Vec3& p) {
        return (p.x > 5.0f) ? -1.0f : std::clamp(p.y - 2.0f, -1.0f, 1.0f);
    };
    CharacterController character;
    character.set_position(Vec3{0.0f, 2.0f, 0.0f});
    for (int i = 0; i < 300; ++i) {
        character.update(density, Vec3{1.0f, 0.0f, 0.0f}, false, kStep);
    }
    CHECK(character.position().x > 2.0f);  // moved toward the wall
    CHECK(character.position().x < 5.2f);  // but was stopped by it
}

TEST_CASE("CharacterController: a prop collider (tree) blocks the player") {
    const DensitySampler density = flat_floor(2.0f);
    CharacterController character;
    character.set_position(Vec3{0.0f, 2.0f, 0.0f});

    Collider tree;
    tree.shape = Collider::Shape::Cylinder;
    tree.center = Vec3{4.0f, 2.0f, 0.0f};
    tree.radius = 1.0f;
    tree.y_min = 1.5f;
    tree.y_max = 6.0f;
    const std::array<Collider, 1> cols{tree};

    for (int i = 0; i < 300; ++i) {
        character.update(density, Vec3{1.0f, 0.0f, 0.0f}, false, kStep, cols);
    }
    CHECK(character.position().x > 1.5f); // moved toward the tree
    CHECK(character.position().x < 2.8f); // stopped at ~4 - (radius + 1.0)
}

TEST_CASE("Collision: box push-out + deterministic world colliders") {
    // A capsule centred inside a thin box is ejected past its own radius.
    Collider box;
    box.shape = Collider::Shape::Box;
    box.center = Vec3{0.0f};
    box.half = Vec2{1.0f, 0.2f};
    box.y_min = 0.0f;
    box.y_max = 3.0f;
    const Vec2 out = resolve_collider(box, Vec2{0.0f, 0.1f}, 0.4f, 1.0f, 1.8f);
    CHECK(glm::length(out) > 0.4f);

    PropLibrary lib;
    CollisionWorld world(1337u, lib);
    std::vector<Collider> a;
    std::vector<Collider> b;
    world.gather(Vec3{40.0f, 0.0f, 40.0f}, a);
    world.gather(Vec3{40.0f, 0.0f, 40.0f}, b);
    CHECK(a.size() == b.size()); // deterministic

    int total = 0;
    bool any_box = false;
    for (int cz = -8; cz < 8; ++cz) {
        for (int cx = -8; cx < 8; ++cx) {
            std::vector<Collider> c;
            world.gather(Vec3{static_cast<f32>(cx) * 8.0f, 0.0f, static_cast<f32>(cz) * 8.0f}, c);
            total += static_cast<int>(c.size());
            for (const Collider& col : c) {
                if (col.shape == Collider::Shape::Box) any_box = true;
            }
        }
    }
    CHECK(total > 0);   // trees/walls exist
    CHECK(any_box);     // at least one house contributed wall colliders
}

TEST_CASE("place_box: a rotated box keeps its orientation (long axis stays long)") {
    // A single wall: long along local-x, thin along local-z, placed at a yaw. With
    // no other colliders around, this isolates the box's orientation - it only
    // behaves correctly if place_box matches the client's R_y mesh transform.
    const f32 yaw = 0.9f;
    const f32 cs = std::cos(yaw);
    const f32 sn = std::sin(yaw);
    const Collider wall = place_box(Vec3{0.0f}, Vec2{2.0f, 0.1f}, 2.0f, 0.0f, Vec3{0.0f}, yaw, 1.0f);
    auto world = [&](f32 lx, f32 lz) { return Vec2{lx * cs + lz * sn, -lx * sn + lz * cs}; };
    auto moved = [&](const Vec2& p) {
        return glm::length(resolve_collider(wall, p, 0.3f, 0.0f, 2.0f) - p);
    };
    CHECK(moved(world(1.5f, 0.0f)) > 0.1f);    // on the wall, along its length -> blocked
    CHECK(moved(world(0.0f, 0.6f)) < 1e-3f);   // off the thin side -> clear
    CHECK(moved(world(2.6f, 0.0f)) < 1e-3f);   // past the end -> clear

    // Centre placement: a box whose local centre is offset, at a world position +
    // yaw, must block at the mesh-transformed world centre (R_y) and be clear back
    // at the prop origin. This pins down the translation half of place_box.
    const Vec3 pos{10.0f, 0.0f, -5.0f};
    const Collider off = place_box(Vec3{3.0f, 0.0f, 0.0f}, Vec2{0.5f, 0.5f}, 2.0f, 0.0f, pos, yaw, 1.0f);
    const Vec2 world_centre{pos.x + 3.0f * cs, pos.z - 3.0f * sn}; // R_y(yaw) * (3,0)
    CHECK(glm::length(resolve_collider(off, world_centre, 0.3f, 0.0f, 2.0f) - world_centre) > 0.1f);
    const Vec2 origin{pos.x, pos.z};
    CHECK(glm::length(resolve_collider(off, origin, 0.3f, 0.0f, 2.0f) - origin) < 1e-3f);
}

TEST_CASE("Forest props: a fallen log has a collider that blocks along its length") {
    PropLibrary lib;
    REQUIRE_FALSE(lib.logs().empty());
    const PropDef& log = lib.logs()[0];
    REQUIRE_FALSE(log.colliders.empty()); // logs are solid; bushes/rocks aren't

    // Place the log rotated and build its world collider the way CollisionWorld
    // does. The log lies along local +X; a point along it blocks, while a point well
    // off to the side (perpendicular) is clear - which only holds if place_box keeps
    // the collider's orientation aligned with the visible (rotated) log.
    const Vec3 pos{6.0f, 0.0f, -3.0f};
    const f32 yaw = 0.8f;
    const BoxCollider& b = log.colliders[0];
    const Collider c = place_box(b.center, b.half_extents, b.height, b.yaw, pos, yaw, 1.0f);
    const f32 cs = std::cos(yaw);
    const f32 sn = std::sin(yaw);
    auto world = [&](f32 lx, f32 lz) { return Vec2{pos.x + lx * cs + lz * sn, pos.z - lx * sn + lz * cs}; };
    auto moved = [&](const Vec2& p) { return glm::length(resolve_collider(c, p, 0.4f, 0.0f, 1.8f) - p); };

    CHECK(moved(world(b.half_extents.x * 0.7f, 0.0f)) > 0.05f);          // along the log -> blocked
    CHECK(moved(world(0.0f, b.half_extents.y + 1.2f)) < 1e-3f);          // off to the side -> clear
    CHECK(moved(world(b.half_extents.x + 1.2f, 0.0f)) < 1e-3f);          // past the end -> clear

    // Bushes are decorative (no collider).
    REQUIRE_FALSE(lib.bushes().empty());
    CHECK(lib.bushes()[0].colliders.empty());
}

TEST_CASE("Projectile: gravity + terrain bounce stays above the floor and settles") {
    const DensitySampler density = flat_floor(2.0f);
    Projectile pr;
    pr.position = Vec3{0.0f, 6.0f, 0.0f};
    pr.velocity = Vec3{5.0f, 0.0f, 0.0f};
    const std::array<Collider, 0> none{};

    f32 min_y = 100.0f;
    for (int i = 0; i < 600 && pr.alive; ++i) {
        step_projectile(pr, density, none, kStep);
        min_y = std::min(min_y, pr.position.y);
    }
    CHECK(pr.position.x > 1.0f);        // travelled forward
    CHECK(min_y > 1.0f);                // never sank through the floor (y = 2)
    CHECK((pr.resting || !pr.alive));   // settled or expired
}

TEST_CASE("Projectile: bounces back off a wall collider") {
    const DensitySampler air = [](const Vec3&) { return 1.0f; }; // no terrain
    Projectile pr;
    pr.position = Vec3{0.0f, 0.0f, 0.0f};
    pr.velocity = Vec3{10.0f, 0.0f, 0.0f};
    Collider wall;
    wall.shape = Collider::Shape::Box;
    wall.center = Vec3{3.0f, 0.0f, 0.0f};
    wall.half = Vec2{0.2f, 4.0f};
    wall.y_min = -2.0f;
    wall.y_max = 2.0f;
    const std::array<Collider, 1> cols{wall};

    for (int i = 0; i < 60; ++i) {
        step_projectile(pr, air, cols, kStep, 0.0f); // no gravity, planar test
    }
    CHECK(pr.velocity.x < 0.0f); // reflected off the wall
    CHECK(pr.position.x < 3.0f); // stayed on the near side
}

TEST_CASE("CharacterController: jump leaves the ground then returns") {
    const DensitySampler density = flat_floor(2.0f);
    CharacterController character;
    character.set_position(Vec3{0.0f, 2.0f, 0.0f});
    character.update(density, Vec3{0.0f}, false, kStep); // settle on ground
    REQUIRE(character.on_ground());

    character.update(density, Vec3{0.0f}, true, kStep); // jump
    CHECK_FALSE(character.on_ground());
    CHECK(character.position().y > 2.0f);

    for (int i = 0; i < 200; ++i) {
        character.update(density, Vec3{0.0f}, false, kStep);
    }
    CHECK(character.on_ground());
    CHECK(character.position().y == doctest::Approx(2.0f).epsilon(0.15));
}
