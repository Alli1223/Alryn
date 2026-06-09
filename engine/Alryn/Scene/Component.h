#pragma once

#include <Alryn/Core/NonCopyable.h>
#include <Alryn/Core/Time.h>

namespace alryn {

class GameObject;
class Event;

// Behaviour/data attached to a GameObject. THIS is where gameplay lives: add a
// feature by subclassing Component and overriding the hooks, then attach it with
// game_object.add_component<MyComponent>(...).
//
// GameObject is a friend so it can wire up ownership and drive the lifecycle.
class Component : public NonCopyable {
    friend class GameObject;

public:
    virtual ~Component() = default;

    GameObject& owner() { return *owner_; }
    const GameObject& owner() const { return *owner_; }

    bool enabled() const { return enabled_; }
    void set_enabled(bool enabled) { enabled_ = enabled; }

protected:
    virtual void on_attach() {}
    virtual void on_update(Timestep dt) { (void)dt; }
    virtual void on_event(Event& event) { (void)event; }
    virtual void on_detach() {}

private:
    GameObject* owner_ = nullptr;
    bool enabled_ = true;
};

} // namespace alryn
