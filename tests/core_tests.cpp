#include <doctest/doctest.h>

#include <Alryn/Core/Assert.h>
#include <Alryn/Core/Event.h>
#include <Alryn/Core/Log.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Core/UUID.h>

#include <unordered_set>

using namespace alryn;

static_assert(sizeof(u8) == 1);
static_assert(sizeof(u16) == 2);
static_assert(sizeof(u32) == 4);
static_assert(sizeof(u64) == 8);
static_assert(sizeof(f32) == 4);
static_assert(sizeof(f64) == 8);

TEST_CASE("Math: angle conversion round-trips") {
    CHECK(degrees(Pi) == doctest::Approx(180.0f));
    CHECK(radians(180.0f) == doctest::Approx(Pi));
    CHECK(degrees(radians(57.0f)) == doctest::Approx(57.0f));
}

TEST_CASE("Math: perspective is Vulkan-correct") {
    const Mat4 p = perspective(radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    // Y is flipped for Vulkan's clip space...
    CHECK(p[1][1] < 0.0f);
    // ...and it's a right-handed perspective (w = -z).
    CHECK(p[2][3] == doctest::Approx(-1.0f));
}

TEST_CASE("Math: identity quaternion leaves vectors unchanged") {
    const Vec3 v{1.0f, 2.0f, 3.0f};
    const Vec3 r = QuatIdentity * v;
    CHECK(r.x == doctest::Approx(v.x));
    CHECK(r.y == doctest::Approx(v.y));
    CHECK(r.z == doctest::Approx(v.z));
}

TEST_CASE("Time: Timestep converts to seconds and milliseconds") {
    const Timestep ts{0.016f};
    CHECK(static_cast<f32>(ts) == doctest::Approx(0.016f));
    CHECK(ts.ms() == doctest::Approx(16.0f));
}

TEST_CASE("Time: Clock is monotonic and non-negative") {
    Clock clock;
    CHECK(clock.elapsed() >= 0.0);
    CHECK(clock.restart() >= 0.0);
}

TEST_CASE("UUID: random ids are valid, unique, and hashable") {
    const UUID a;
    const UUID b;
    CHECK(a.valid());
    CHECK(b.valid());
    CHECK(a != b); // 1 in 2^64 chance of collision

    const UUID zero{0};
    CHECK_FALSE(zero.valid());

    const UUID copy{a.value()};
    CHECK(copy == a);

    const std::hash<UUID> hasher;
    CHECK(hasher(a) == std::hash<u64>{}(a.value()));

    std::unordered_set<UUID> set;
    for (int i = 0; i < 1000; ++i) {
        set.insert(UUID{});
    }
    CHECK(set.size() == 1000); // all distinct
}

namespace {

class CloseEvent : public Event {
public:
    ALRYN_EVENT_CLASS_TYPE(WindowClose)
    ALRYN_EVENT_CLASS_CATEGORY(EventCategoryApplication)
};

class MoveEvent : public Event {
public:
    ALRYN_EVENT_CLASS_TYPE(MouseMoved)
    ALRYN_EVENT_CLASS_CATEGORY(EventCategoryInput | EventCategoryMouse)
};

} // namespace

TEST_CASE("Event: type, category, and dispatch") {
    CloseEvent close;
    CHECK(close.type() == EventType::WindowClose);
    CHECK(close.in_category(EventCategoryApplication));
    CHECK_FALSE(close.in_category(EventCategoryInput));
    CHECK_FALSE(close.handled);

    SUBCASE("matching handler consumes the event") {
        bool ran = false;
        EventDispatcher dispatcher{close};
        const bool matched = dispatcher.dispatch<CloseEvent>([&](CloseEvent&) {
            ran = true;
            return true;
        });
        CHECK(matched);
        CHECK(ran);
        CHECK(close.handled);
    }

    SUBCASE("non-matching handler is skipped") {
        EventDispatcher dispatcher{close};
        const bool matched = dispatcher.dispatch<MoveEvent>([](MoveEvent&) { return true; });
        CHECK_FALSE(matched);
        CHECK_FALSE(close.handled);
    }
}

TEST_CASE("Assert: VERIFY passes for true conditions") {
    int x = 5;
    ALRYN_VERIFY(x == 5);          // must not abort
    ALRYN_VERIFY(x > 0, "x={}", x); // formatted message variant
    CHECK(x == 5);
}

TEST_CASE("Log: formatting macros compile and run") {
    ALRYN_INFO("value = {}, pi = {:.2f}", 42, Pi); // suppressed at Warn level
    ALRYN_WARN("a warning with {} args", 1);
    CHECK(true);
}
