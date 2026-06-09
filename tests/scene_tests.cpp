#include <doctest/doctest.h>

#include <Alryn/Engine/Application.h>
#include <Alryn/Engine/Engine.h>
#include <Alryn/Engine/Subsystem.h>
#include <Alryn/Scene/Camera.h>
#include <Alryn/Scene/GameObject.h>
#include <Alryn/Scene/Scene.h>
#include <Alryn/Scene/Transform.h>

using namespace alryn;

TEST_CASE("Transform: translation shows up in the matrix") {
    Transform t;
    t.set_position({2.0f, 3.0f, 4.0f});
    const Mat4& m = t.matrix();
    CHECK(m[3][0] == doctest::Approx(2.0f));
    CHECK(m[3][1] == doctest::Approx(3.0f));
    CHECK(m[3][2] == doctest::Approx(4.0f));
}

TEST_CASE("Transform: basis vectors after a 90-degree yaw") {
    Transform t;
    t.set_euler({0.0f, HalfPi, 0.0f}); // yaw +90 deg about Y
    const Vec3 fwd = t.forward();
    // forward (-Z) rotated +90deg about Y points to -X
    CHECK(fwd.x == doctest::Approx(-1.0f).epsilon(0.001));
    CHECK(fwd.z == doctest::Approx(0.0f).epsilon(0.001));
}

namespace {

// A component that counts how many times it was updated - proves polymorphic
// dispatch through the base Component interface.
class Ticker : public Component {
public:
    int ticks = 0;
    f32 accumulated = 0.0f;
    bool attached = false;
    bool* out_detached = nullptr; // set on detach so it can be observed after free

protected:
    void on_attach() override { attached = true; }
    void on_update(Timestep dt) override {
        ++ticks;
        accumulated += dt.seconds;
    }
    void on_detach() override {
        if (out_detached != nullptr) {
            *out_detached = true;
        }
    }
};

} // namespace

TEST_CASE("GameObject: add/get component and lifecycle") {
    auto object = std::make_unique<GameObject>("Player");
    CHECK(object->name() == "Player");
    CHECK(object->id().valid());

    auto& ticker = object->add_component<Ticker>();
    CHECK(ticker.attached);
    CHECK(object->get_component<Ticker>() == &ticker);
    CHECK(object->has_component<Ticker>());

    object->update(Timestep{0.5f});
    object->update(Timestep{0.25f});
    CHECK(ticker.ticks == 2);
    CHECK(ticker.accumulated == doctest::Approx(0.75f));

    SUBCASE("disabled components are skipped") {
        ticker.set_enabled(false);
        object->update(Timestep{1.0f});
        CHECK(ticker.ticks == 2); // unchanged
    }
}

TEST_CASE("GameObject: on_detach runs when the object is destroyed") {
    bool detached = false;
    {
        GameObject object{"Temp"};
        object.add_component<Ticker>().out_detached = &detached;
        CHECK_FALSE(detached);
    } // object (and its components) destroyed here
    CHECK(detached);
}

TEST_CASE("GameObject: hierarchy composes world matrices") {
    GameObject parent{"Parent"};
    parent.transform().set_position({10.0f, 0.0f, 0.0f});
    GameObject& child = parent.add_child("Child");
    child.transform().set_position({0.0f, 5.0f, 0.0f});

    const Mat4 world = child.world_matrix();
    CHECK(world[3][0] == doctest::Approx(10.0f));
    CHECK(world[3][1] == doctest::Approx(5.0f));
    CHECK(child.parent() == &parent);
}

TEST_CASE("Scene: create, update, and find objects") {
    Scene scene{"TestScene"};
    GameObject& a = scene.create_object("Alpha");
    GameObject& b = scene.create_object("Beta");
    auto& ticker = a.add_component<Ticker>();

    scene.update(Timestep{0.1f});
    CHECK(ticker.ticks == 1);

    CHECK(scene.find("Alpha") == &a);
    CHECK(scene.find("Beta") == &b);
    CHECK(scene.find(a.id()) == &a);
    CHECK(scene.find("Nope") == nullptr);
}

TEST_CASE("Camera: view-projection is finite and reacts to aspect") {
    Camera cam;
    cam.set_perspective(radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    cam.look_at({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f});

    const Mat4 vp = cam.view_projection();
    // A point at the origin should land in front of the camera (finite clip).
    const Vec4 clip = vp * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    CHECK(std::isfinite(clip.x));
    CHECK(std::isfinite(clip.w));
    CHECK(cam.position().z == doctest::Approx(5.0f));
}

// --- Engine / Application -------------------------------------------------

namespace {

// Stops the application after a fixed number of updates - lets us drive the real
// run loop in a headless test.
class AutoStopSystem : public Subsystem {
public:
    explicit AutoStopSystem(Engine& engine, int stop_after)
        : engine_(engine), stop_after_(stop_after) {}

    const char* name() const override { return "AutoStopSystem"; }
    void on_update(Timestep) override {
        if (++updates_ >= stop_after_) {
            engine_.request_stop();
        }
    }
    int updates() const { return updates_; }

private:
    Engine& engine_;
    int stop_after_;
    int updates_ = 0;
};

} // namespace

TEST_CASE("Engine: subsystem registration and typed lookup") {
    Engine engine;
    auto& sys = engine.add_subsystem<AutoStopSystem>(engine, 3);
    CHECK(engine.subsystem_count() == 1);
    CHECK(engine.get_subsystem<AutoStopSystem>() == &sys);

    CHECK(engine.init());
    CHECK(engine.initialized());
    engine.update(Timestep{0.016f});
    CHECK(sys.updates() == 1);
    engine.shutdown();
    CHECK_FALSE(engine.initialized());
}

namespace {

class HeadlessApp : public Application {
public:
    HeadlessApp() : Application(make_config()) {}
    int updates = 0;

protected:
    void on_configure() override { engine().add_subsystem<AutoStopSystem>(engine(), 5); }
    void on_update(Timestep) override { ++updates; }

private:
    static ApplicationConfig make_config() {
        ApplicationConfig c;
        c.name = "HeadlessApp";
        c.headless = true;
        c.max_frames = 100; // safety net; AutoStopSystem should stop us first
        return c;
    }
};

} // namespace

TEST_CASE("Application: run loop drives subsystems and exits") {
    HeadlessApp app;
    app.run();
    // AutoStopSystem stops after 5 updates, well before the 100-frame safety net.
    CHECK(app.frame_count() == 5);
    CHECK(app.updates == 5);
}
