#include <doctest/doctest.h>

#include <Alryn/UI/UIContext.h>
#include <Alryn/UI/VectorFont.h>
#include <Alryn/UI/Widget.h>
#include <Alryn/UI/Widgets.h>

#include <string>

using namespace alryn;
using namespace alryn::ui;

// The UI is logic-tested headlessly (no GPU/DrawList needed): hit-testing, event
// routing through the widget tree, and each widget's behaviour.

TEST_CASE("Rect::contains") {
    Rect r{10.0f, 20.0f, 100.0f, 40.0f};
    CHECK(r.contains(Vec2{10.0f, 20.0f}));   // top-left corner (inclusive)
    CHECK(r.contains(Vec2{60.0f, 40.0f}));   // interior
    CHECK(r.contains(Vec2{110.0f, 60.0f}));  // bottom-right corner
    CHECK_FALSE(r.contains(Vec2{9.0f, 40.0f}));
    CHECK_FALSE(r.contains(Vec2{60.0f, 61.0f}));
}

TEST_CASE("vector font metrics") {
    CHECK(font_text_width("", 32.0f) == doctest::Approx(0.0f));
    CHECK(font_text_width("HELLO", 32.0f) > 0.0f);
    // Wider strings are wider.
    CHECK(font_text_width("HELLO WORLD", 32.0f) > font_text_width("HELLO", 32.0f));
    // Scales with size.
    CHECK(font_text_width("A", 64.0f) == doctest::Approx(2.0f * font_text_width("A", 32.0f)));
    // Known glyphs have strokes; an unknown one is blank; lowercase maps to upper.
    CHECK_FALSE(font_glyph('A').strokes.empty());
    CHECK_FALSE(font_glyph('5').strokes.empty());
    CHECK(font_glyph('~').strokes.empty());
    CHECK(font_glyph('a').strokes.size() == font_glyph('A').strokes.size());
}

TEST_CASE("Button click fires on press-and-release inside") {
    int clicks = 0;
    Button button{"OK", [&] { ++clicks; }};
    button.bounds = Rect{0.0f, 0.0f, 100.0f, 40.0f};

    // Press inside, release inside -> click.
    CHECK(button.dispatch_pointer_down(Vec2{50.0f, 20.0f}, 0));
    CHECK(button.dispatch_pointer_up(Vec2{50.0f, 20.0f}, 0));
    CHECK(clicks == 1);

    // Press inside, release outside -> no click (drag-off cancels).
    CHECK(button.dispatch_pointer_down(Vec2{50.0f, 20.0f}, 0));
    button.dispatch_pointer_up(Vec2{500.0f, 500.0f}, 0);
    CHECK(clicks == 1);

    // Press outside -> not consumed, no click.
    CHECK_FALSE(button.dispatch_pointer_down(Vec2{500.0f, 20.0f}, 0));
    CHECK(clicks == 1);
}

TEST_CASE("disabled button ignores input") {
    int clicks = 0;
    Button button{"OK", [&] { ++clicks; }};
    button.bounds = Rect{0.0f, 0.0f, 100.0f, 40.0f};
    button.enabled = false;
    CHECK_FALSE(button.dispatch_pointer_down(Vec2{50.0f, 20.0f}, 0));
    button.dispatch_pointer_up(Vec2{50.0f, 20.0f}, 0);
    CHECK(clicks == 0);
}

TEST_CASE("event routing through the widget tree hits the right child") {
    Widget root;
    root.bounds = Rect{0.0f, 0.0f, 400.0f, 400.0f};

    int a_clicks = 0;
    int b_clicks = 0;
    auto& a = root.add<Button>("A", [&] { ++a_clicks; });
    a.bounds = Rect{0.0f, 0.0f, 100.0f, 40.0f};
    auto& b = root.add<Button>("B", [&] { ++b_clicks; });
    b.bounds = Rect{0.0f, 200.0f, 100.0f, 40.0f};

    CHECK(root.dispatch_pointer_down(Vec2{50.0f, 220.0f}, 0));
    CHECK(root.dispatch_pointer_up(Vec2{50.0f, 220.0f}, 0));
    CHECK(a_clicks == 0);
    CHECK(b_clicks == 1);
}

TEST_CASE("Toggle flips and reports its value") {
    bool last = false;
    int changes = 0;
    Toggle toggle{"VSYNC", false, [&](bool v) { last = v; ++changes; }};
    toggle.bounds = Rect{0.0f, 0.0f, 200.0f, 40.0f};

    CHECK(toggle.dispatch_pointer_down(Vec2{20.0f, 20.0f}, 0));
    CHECK(toggle.value);
    CHECK(last);
    CHECK(changes == 1);
    toggle.dispatch_pointer_down(Vec2{20.0f, 20.0f}, 0);
    CHECK_FALSE(toggle.value);
    CHECK_FALSE(last);
    CHECK(changes == 2);
}

TEST_CASE("Slider maps the pointer to a clamped, optionally-integer value") {
    f32 reported = -1.0f;
    Slider slider{"RENDER DISTANCE", 4.0f, 0.0f, 10.0f, [&](f32 v) { reported = v; }};
    slider.bounds = Rect{0.0f, 0.0f, 200.0f, 50.0f}; // track spans x in [0,200]
    slider.integer = true;

    // Click at the middle of the track -> 5.
    CHECK(slider.dispatch_pointer_down(Vec2{100.0f, 25.0f}, 0));
    CHECK(slider.value == doctest::Approx(5.0f));
    CHECK(reported == doctest::Approx(5.0f));

    // Drag past the right edge -> clamped to max.
    slider.dispatch_pointer_move(Vec2{500.0f, 25.0f});
    CHECK(slider.value == doctest::Approx(10.0f));
    slider.dispatch_pointer_up(Vec2{500.0f, 25.0f}, 0);

    // After release, moves no longer change the value.
    slider.dispatch_pointer_move(Vec2{0.0f, 25.0f});
    CHECK(slider.value == doctest::Approx(10.0f));
}

TEST_CASE("Stepper cycles options with wrap-around") {
    usize idx = 0;
    Stepper stepper{"RESOLUTION", {"A", "B", "C"}, 0, [&](usize i) { idx = i; }};
    stepper.bounds = Rect{0.0f, 0.0f, 300.0f, 50.0f};
    const Vec2 right{stepper.bounds.x + stepper.bounds.w - 15.0f, 25.0f};
    const Vec2 left{stepper.bounds.x + stepper.bounds.w - 165.0f, 25.0f};

    CHECK(stepper.dispatch_pointer_down(right, 0));
    CHECK(stepper.index == 1);
    CHECK(idx == 1);
    stepper.dispatch_pointer_down(right, 0); // -> 2
    stepper.dispatch_pointer_down(right, 0); // -> 0 (wrap)
    CHECK(stepper.index == 0);
    stepper.dispatch_pointer_down(left, 0); // -> 2 (wrap back)
    CHECK(stepper.index == 2);
}

TEST_CASE("SwatchRow selects the clicked colour") {
    usize picked = 0;
    SwatchRow row{{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}, Vec3{1, 1, 0}}, 0,
                  [&](usize i) { picked = i; }};
    row.bounds = Rect{0.0f, 0.0f, 400.0f, 40.0f}; // four swatches across 400px
    row.gap = 0.0f;                                // cells are exactly 100px wide

    CHECK(row.dispatch_pointer_down(Vec2{250.0f, 20.0f}, 0)); // third cell [200,300)
    CHECK(row.index == 2);
    CHECK(picked == 2);
    CHECK(row.dispatch_pointer_down(Vec2{50.0f, 20.0f}, 0)); // first cell
    CHECK(row.index == 0);
}

TEST_CASE("TextField focus, filtered text entry and backspace") {
    std::string value;
    TextField field;
    field.bounds = Rect{0.0f, 0.0f, 200.0f, 40.0f};
    field.on_change = [&](const std::string& s) { value = s; };
    field.filter = [](char c) { return (c >= '0' && c <= '9') || c == '.'; };

    // Not focused: text is ignored.
    CHECK_FALSE(field.dispatch_text('1'));
    CHECK(field.text.empty());

    // Click to focus.
    CHECK(field.dispatch_pointer_down(Vec2{10.0f, 20.0f}, 0));
    CHECK(field.focused);

    CHECK(field.dispatch_text('1'));
    CHECK(field.dispatch_text('2'));
    CHECK(field.dispatch_text('7'));
    CHECK(field.dispatch_text('.'));
    CHECK_FALSE(field.dispatch_text('x')); // filtered out
    CHECK(field.text == "127.");
    CHECK(value == "127.");

    constexpr KeyCode kBackspace = 259;
    CHECK(field.dispatch_key(kBackspace));
    CHECK(field.text == "127");

    // Clicking outside drops focus.
    field.dispatch_pointer_down(Vec2{500.0f, 500.0f}, 0);
    CHECK_FALSE(field.focused);
}

TEST_CASE("UIContext routes events to its root tree") {
    UIContext ui;
    ui.set_screen(800.0f, 600.0f);
    CHECK(ui.screen() == Vec2{800.0f, 600.0f});

    int clicks = 0;
    auto& button = ui.root().add<Button>("PLAY", [&] { ++clicks; });
    button.bounds = Rect{100.0f, 100.0f, 200.0f, 50.0f};

    CHECK(ui.pointer_down(Vec2{150.0f, 120.0f}, 0));
    CHECK(ui.pointer_up(Vec2{150.0f, 120.0f}, 0));
    CHECK(clicks == 1);

    // A click in empty space is not consumed.
    CHECK_FALSE(ui.pointer_down(Vec2{10.0f, 10.0f}, 0));
}

TEST_CASE("UIContext focus navigation (controller/keyboard): order, skip, activate, adjust, wrap") {
    UIContext ui;
    ui.set_screen(800.0f, 600.0f);

    int a_clicks = 0;
    int b_clicks = 0;
    auto& title = ui.root().add<Label>("TITLE", 30.0f); // a Label is not focusable - it's skipped
    title.bounds = Rect{100.0f, 40.0f, 200.0f, 40.0f};
    auto& a = ui.root().add<Button>("A", [&] { ++a_clicks; });
    a.bounds = Rect{100.0f, 100.0f, 200.0f, 50.0f};
    auto& step = ui.root().add<Stepper>("OPT", std::vector<std::string>{"X", "Y", "Z"}, 0);
    step.bounds = Rect{100.0f, 200.0f, 300.0f, 50.0f};
    auto& b = ui.root().add<Button>("B", [&] { ++b_clicks; });
    b.bounds = Rect{100.0f, 300.0f, 200.0f, 50.0f};

    // A frame's update() establishes the focusable count and starts with nothing focused (no ring).
    ui.update(0.016f, Vec2{-1.0f, -1.0f});
    CHECK_FALSE(ui.has_focus());

    // First "down" lands on the top-most focusable control (Button A) - the Label is skipped.
    ui.focus_move(+1);
    CHECK(ui.has_focus());
    ui.focus_activate();
    CHECK(a_clicks == 1);

    // Down moves to the Stepper; left/right adjust it in place without firing a button.
    ui.focus_move(+1);
    CHECK(step.index == 0);
    ui.focus_nav(+1);
    CHECK(step.index == 1);
    ui.focus_nav(-1);
    CHECK(step.index == 0);
    CHECK(a_clicks == 1);

    // Down to Button B and activate it.
    ui.focus_move(+1);
    ui.focus_activate();
    CHECK(b_clicks == 1);

    // Wrap-around: from the last control, "down" returns to the first.
    ui.focus_move(+1);
    ui.focus_activate();
    CHECK(a_clicks == 2);

    // "Up" from the first wraps to the last.
    ui.focus_move(-1);
    ui.focus_activate();
    CHECK(b_clicks == 2);

    // A menu rebuild (the focusable set changes) drops the now-stale focus on the next frame.
    ui.root().clear_children();
    ui.update(0.016f, Vec2{-1.0f, -1.0f});
    CHECK_FALSE(ui.has_focus());
}
