#pragma once

#include <Alryn/Core/NonCopyable.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Engine/Engine.h>

#include <memory>
#include <string>

namespace alryn {

class Event;
class Window;
class Renderer;
class Input;

struct ApplicationConfig {
    std::string name = "Alryn Application";
    u32 width = 1280;
    u32 height = 720;

    bool headless = false;          // no window/renderer (dedicated server, tests, CI)
    bool enable_validation = true;  // Vulkan validation layers in debug builds

    // 0 = run until stopped. >0 = run exactly N frames then stop (handy for
    // headless smoke tests and deterministic CI runs).
    u64 max_frames = 0;
};

// Base class for a game/tool built on Alryn. Derive it and override the virtual
// hooks. Owns the Engine and drives the main loop.
//
// Lifecycle (see run()):
//   on_configure() -> register subsystems
//   engine.init()  -> bring subsystems up
//   on_init()      -> build your scene
//   [loop] engine.update(dt) -> on_update(dt) -> on_render()
//   on_shutdown()  -> your cleanup
//   engine.shutdown()
class Application : public NonCopyable {
public:
    explicit Application(ApplicationConfig config = {});
    virtual ~Application();

    void run();
    void close() { engine_->request_stop(); }

    Engine& engine() { return *engine_; }
    const Engine& engine() const { return *engine_; }
    const ApplicationConfig& config() const { return config_; }
    u64 frame_count() const { return frame_; }

    // The windowed renderer/input, or nullptr in headless mode.
    Renderer* renderer() { return renderer_; }
    Window* window() { return window_.get(); }
    Input* input() { return input_; }

    static Application& get() { return *s_instance; }

protected:
    virtual void on_configure() {}
    virtual void on_init() {}
    virtual void on_update(Timestep dt) { (void)dt; }
    virtual void on_render() {}
    virtual void on_event(Event& event) { (void)event; }
    virtual void on_shutdown() {}

    // Routes a platform/event-source event through the engine then the app.
    void handle_event(Event& event);

private:
    ApplicationConfig config_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<Window> window_;  // windowed mode only
    Renderer* renderer_ = nullptr;    // owned by the engine's subsystem list
    Input* input_ = nullptr;          // owned by the engine's subsystem list
    u64 frame_ = 0;

    static Application* s_instance;
};

} // namespace alryn
