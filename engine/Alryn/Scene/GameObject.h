#pragma once

#include <Alryn/Core/NonCopyable.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Core/UUID.h>
#include <Alryn/Scene/Component.h>
#include <Alryn/Scene/Transform.h>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace alryn {

class Event;

// A node in the scene graph: a Transform, a set of Components, and child
// GameObjects. Owns its components and children. Non-copyable (stable identity).
class GameObject : public NonCopyable {
public:
    explicit GameObject(std::string name = "GameObject");
    ~GameObject();

    UUID id() const { return id_; }
    const std::string& name() const { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    bool active() const { return active_; }
    void set_active(bool active) { active_ = active; }

    Transform& transform() { return transform_; }
    const Transform& transform() const { return transform_; }

    template <typename T, typename... Args>
    T& add_component(Args&&... args) {
        static_assert(std::is_base_of_v<Component, T>, "T must derive from Component");
        auto component = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *component;
        Component& base = ref; // friendship is with Component, not T: drive the
        base.owner_ = this;    // lifecycle through the base so access checks pass
        components_.push_back(std::move(component));
        base.on_attach(); // virtual-dispatches to T::on_attach
        return ref;
    }

    template <typename T>
    T* get_component() {
        for (auto& component : components_) {
            if (auto* typed = dynamic_cast<T*>(component.get())) {
                return typed;
            }
        }
        return nullptr;
    }

    template <typename T>
    bool has_component() {
        return get_component<T>() != nullptr;
    }

    // Hierarchy --------------------------------------------------------------
    GameObject& add_child(std::string name = "GameObject");
    GameObject* parent() const { return parent_; }
    const std::vector<std::unique_ptr<GameObject>>& children() const { return children_; }

    // World matrix = parent world matrix * local matrix.
    Mat4 world_matrix() const;

    // Recursively updates components then children (skips inactive subtrees).
    void update(Timestep dt);
    void dispatch_event(Event& event);

private:
    UUID id_{};
    std::string name_;
    bool active_ = true;
    Transform transform_;

    GameObject* parent_ = nullptr;
    std::vector<std::unique_ptr<Component>> components_;
    std::vector<std::unique_ptr<GameObject>> children_;
};

} // namespace alryn
