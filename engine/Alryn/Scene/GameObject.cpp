#include <Alryn/Scene/GameObject.h>

#include <Alryn/Core/Event.h>

namespace alryn {

GameObject::GameObject(std::string name) : name_(std::move(name)) {}

GameObject::~GameObject() {
    for (auto& component : components_) {
        component->on_detach();
    }
}

GameObject& GameObject::add_child(std::string name) {
    auto child = std::make_unique<GameObject>(std::move(name));
    child->parent_ = this;
    GameObject& ref = *child;
    children_.push_back(std::move(child));
    return ref;
}

Mat4 GameObject::world_matrix() const {
    if (parent_ != nullptr) {
        return parent_->world_matrix() * transform_.matrix();
    }
    return transform_.matrix();
}

void GameObject::update(Timestep dt) {
    if (!active_) {
        return;
    }
    for (auto& component : components_) {
        if (component->enabled()) {
            component->on_update(dt);
        }
    }
    for (auto& child : children_) {
        child->update(dt);
    }
}

void GameObject::dispatch_event(Event& event) {
    if (!active_) {
        return;
    }
    for (auto& component : components_) {
        if (component->enabled()) {
            component->on_event(event);
        }
    }
    for (auto& child : children_) {
        child->dispatch_event(event);
    }
}

} // namespace alryn
