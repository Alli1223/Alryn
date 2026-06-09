#include <Alryn/Scene/Scene.h>

#include <utility>

namespace alryn {

namespace {

template <typename Pred>
GameObject* dfs(GameObject& node, Pred&& pred) {
    if (pred(node)) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (GameObject* found = dfs(*child, pred)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

Scene::Scene(std::string name)
    : name_(std::move(name)), root_(std::make_unique<GameObject>("<root>")) {}

GameObject* Scene::find(UUID id) {
    return dfs(*root_, [id](const GameObject& node) { return node.id() == id; });
}

GameObject* Scene::find(std::string_view name) {
    return dfs(*root_, [name](const GameObject& node) { return node.name() == name; });
}

} // namespace alryn
