#pragma once

#include <Alryn/Core/NonCopyable.h>
#include <Alryn/Core/Time.h>

namespace alryn {

class Engine;
class Event;

// A long-lived engine service with a lifecycle. Renderer, Window, Networking,
// Physics, Audio... all derive from this. The Engine owns them, initialises them
// in registration order, updates them each frame, and shuts them down in reverse.
//
// This is one of the engine's primary extension points: add a capability by
// subclassing Subsystem and registering it with engine.add_subsystem<T>().
class Subsystem : public NonCopyable {
public:
    virtual ~Subsystem() = default;

    // Human-readable name, used in logs.
    virtual const char* name() const = 0;

    // Return false to abort engine startup.
    virtual bool on_init(Engine& engine) {
        (void)engine;
        return true;
    }

    virtual void on_shutdown() {}
    virtual void on_update(Timestep dt) { (void)dt; }
    virtual void on_event(Event& event) { (void)event; }
};

} // namespace alryn
