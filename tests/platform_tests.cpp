#include <doctest/doctest.h>

#include <Alryn/Core/Event.h>
#include <Alryn/Platform/Events.h>
#include <Alryn/Platform/Window.h>

#include <string>

using namespace alryn;

TEST_CASE("Events: concrete types carry data, category, and dispatch") {
    KeyPressedEvent key{65, false}; // 'A'
    CHECK(key.type() == EventType::KeyPressed);
    CHECK(key.key() == 65);
    CHECK(key.in_category(EventCategoryKeyboard));
    CHECK(key.in_category(EventCategoryInput));
    CHECK_FALSE(key.in_category(EventCategoryMouse));

    Event& base = key;
    EventDispatcher dispatcher{base};
    bool handled = false;
    dispatcher.dispatch<KeyPressedEvent>([&](KeyPressedEvent& e) {
        handled = (e.key() == 65);
        return true;
    });
    CHECK(handled);
    CHECK(base.handled);

    WindowResizeEvent resize{1920, 1080};
    CHECK(resize.width() == 1920);
    CHECK(resize.height() == 1080);
    CHECK(resize.to_string().find("1920") != std::string::npos);

    MouseButtonPressedEvent click{0};
    CHECK(click.in_category(EventCategoryMouseButton));
    CHECK(click.type() == EventType::MouseButtonPressed);
}

// Skips when there's no display (CI/headless), so it never fails spuriously.
TEST_CASE("Window: create + Vulkan extensions, or skip if no display") {
    if (!Window::platform_available()) {
        MESSAGE("No windowing platform (headless) - skipping window test");
        return;
    }

    Window window;
    WindowConfig config;
    config.title = "alryn-test";
    config.width = 320;
    config.height = 240;
    config.resizable = false;
    REQUIRE(window.create(config));
    CHECK(window.native() != nullptr);

    const auto extensions = Window::required_instance_extensions();
    CHECK_FALSE(extensions.empty()); // platform must advertise VK_KHR_surface + a WSI ext

    window.poll_events();
    CHECK_FALSE(window.should_close());
    window.set_should_close(true);
    CHECK(window.should_close());

    window.destroy();
}
