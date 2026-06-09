#include <Alryn/Engine/Application.h>

#include <Alryn/Core/Event.h>
#include <Alryn/Core/Log.h>
#include <Alryn/Platform/Events.h>
#include <Alryn/Platform/Input.h>
#include <Alryn/Platform/Window.h>
#include <Alryn/Renderer/Renderer.h>

#include <utility>

namespace alryn {

Application* Application::s_instance = nullptr;

Application::Application(ApplicationConfig config)
    : config_(std::move(config)), engine_(std::make_unique<Engine>()) {
    s_instance = this;
    Log::init();
    ALRYN_INFO("Alryn Engine - application '{}' ({})", config_.name,
               config_.headless ? "headless" : "windowed");
}

Application::~Application() {
    s_instance = nullptr;
}

void Application::handle_event(Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.dispatch<WindowCloseEvent>([this](WindowCloseEvent&) {
        close();
        return true;
    });
    dispatcher.dispatch<WindowResizeEvent>([this](WindowResizeEvent&) {
        if (renderer_ != nullptr) {
            renderer_->request_resize();
        }
        return false; // let the game react to resizes too
    });

    engine_->dispatch_event(event);
    if (!event.handled) {
        on_event(event);
    }
}

void Application::run() {
    // Windowed apps: open a window and attach the renderer subsystem. If no
    // display is available we transparently fall back to a headless run.
    if (!config_.headless) {
        window_ = std::make_unique<Window>();
        WindowConfig window_config;
        window_config.title = config_.name;
        window_config.width = config_.width;
        window_config.height = config_.height;
        if (window_->create(window_config)) {
            window_->set_event_callback([this](Event& event) { handle_event(event); });
            input_ = &engine_->add_subsystem<Input>();
            RendererConfig renderer_config;
            renderer_config.enable_validation = config_.enable_validation;
            renderer_ = &engine_->add_subsystem<Renderer>(*window_, renderer_config);
        } else {
            ALRYN_WARN("No window/display available - continuing headless");
            window_.reset();
        }
    }

    on_configure();

    if (!engine_->init()) {
        ALRYN_FATAL("Engine initialisation failed - aborting run");
        return;
    }

    on_init();
    engine_->set_running(true);

    Clock clock;
    while (engine_->running()) {
        if (window_ != nullptr) {
            window_->poll_events();
        }

        const Timestep dt{static_cast<f32>(clock.restart())};
        engine_->update(dt);
        on_update(dt);

        if (renderer_ != nullptr) {
            if (renderer_->begin_frame()) {
                on_render();
                renderer_->end_frame();
            }
        } else {
            on_render();
        }

        ++frame_;
        if (config_.max_frames != 0 && frame_ >= config_.max_frames) {
            engine_->request_stop();
        }
        if (window_ != nullptr && window_->should_close()) {
            engine_->request_stop();
        }
    }

    on_shutdown();        // game frees its GPU resources while the device is alive
    engine_->shutdown();  // renderer.on_shutdown tears down Vulkan
    renderer_ = nullptr;
    window_.reset();
    ALRYN_INFO("Application '{}' exited cleanly after {} frame(s)", config_.name, frame_);
}

} // namespace alryn
