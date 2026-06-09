#pragma once

#include <Alryn/Core/NonCopyable.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Engine/Subsystem.h>

#include <memory>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace alryn {

class Event;

// Owns and orchestrates the engine's subsystems. Subsystems are stored in an
// ordered list (init/update order) plus a type-indexed map for fast lookup:
//
//   engine.add_subsystem<Renderer>(config);
//   Renderer* r = engine.get_subsystem<Renderer>();
//
// The Engine drives no main loop itself; Application does. Engine just sequences
// subsystem lifecycle and fan-out of update/event calls.
class Engine : public NonCopyable {
public:
    Engine();
    ~Engine();

    template <typename T, typename... Args>
    T& add_subsystem(Args&&... args) {
        static_assert(std::is_base_of_v<Subsystem, T>, "T must derive from Subsystem");
        auto subsystem = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *subsystem;
        lookup_[std::type_index(typeid(T))] = subsystem.get();
        subsystems_.push_back(std::move(subsystem));
        if (initialized_) {
            ref.on_init(*this); // late registration after startup
        }
        return ref;
    }

    template <typename T>
    T* get_subsystem() {
        const auto it = lookup_.find(std::type_index(typeid(T)));
        return it == lookup_.end() ? nullptr : static_cast<T*>(it->second);
    }

    bool init();
    void shutdown();
    void update(Timestep dt);
    void dispatch_event(Event& event);

    bool running() const { return running_; }
    void set_running(bool running) { running_ = running; }
    void request_stop() { running_ = false; }
    bool initialized() const { return initialized_; }

    usize subsystem_count() const { return subsystems_.size(); }

private:
    std::vector<std::unique_ptr<Subsystem>> subsystems_;
    std::unordered_map<std::type_index, Subsystem*> lookup_;
    bool initialized_ = false;
    bool running_ = false;
};

} // namespace alryn
