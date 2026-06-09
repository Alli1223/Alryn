#include <Alryn/Engine/Engine.h>

#include <Alryn/Core/Event.h>
#include <Alryn/Core/Log.h>

namespace alryn {

Engine::Engine() = default;

Engine::~Engine() {
    if (initialized_) {
        shutdown();
    }
}

bool Engine::init() {
    if (initialized_) {
        return true;
    }
    for (auto& subsystem : subsystems_) {
        ALRYN_INFO("Initialising subsystem: {}", subsystem->name());
        if (!subsystem->on_init(*this)) {
            ALRYN_ERROR("Subsystem '{}' failed to initialise", subsystem->name());
            return false;
        }
    }
    initialized_ = true;
    return true;
}

void Engine::shutdown() {
    if (!initialized_) {
        return;
    }
    // Reverse order so dependents tear down before their dependencies.
    for (auto it = subsystems_.rbegin(); it != subsystems_.rend(); ++it) {
        ALRYN_INFO("Shutting down subsystem: {}", (*it)->name());
        (*it)->on_shutdown();
    }
    initialized_ = false;
}

void Engine::update(Timestep dt) {
    for (auto& subsystem : subsystems_) {
        subsystem->on_update(dt);
    }
}

void Engine::dispatch_event(Event& event) {
    for (auto& subsystem : subsystems_) {
        subsystem->on_event(event);
        if (event.handled) {
            break;
        }
    }
}

} // namespace alryn
