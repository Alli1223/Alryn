#pragma once

#include <Alryn/Core/NonCopyable.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/UUID.h>
#include <Alryn/Scene/GameObject.h>

#include <memory>
#include <string>
#include <string_view>

namespace alryn {

class Event;

// Owns a hierarchy of GameObjects rooted at an implicit root node. The same
// Scene type is intended to run on both client and (later) server.
class Scene : public NonCopyable {
public:
    explicit Scene(std::string name = "Scene");

    const std::string& name() const { return name_; }

    GameObject& root() { return *root_; }
    const GameObject& root() const { return *root_; }

    // Creates a top-level object (child of the root).
    GameObject& create_object(std::string name = "GameObject") {
        return root_->add_child(std::move(name));
    }

    void update(Timestep dt) { root_->update(dt); }
    void dispatch_event(Event& event) { root_->dispatch_event(event); }

    // Depth-first search through the whole graph.
    GameObject* find(UUID id);
    GameObject* find(std::string_view name);

private:
    std::string name_;
    std::unique_ptr<GameObject> root_;
};

} // namespace alryn
