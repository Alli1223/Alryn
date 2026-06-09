#include <doctest/doctest.h>

#include <Alryn/Core/Density.h>
#include <Alryn/Physics/CharacterController.h>
#include <Alryn/Platform/Events.h>
#include <Alryn/Platform/Input.h>

#include <algorithm>

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
