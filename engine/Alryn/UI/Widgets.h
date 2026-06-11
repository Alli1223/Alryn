#pragma once

#include <Alryn/UI/DrawList.h>
#include <Alryn/UI/Theme.h>
#include <Alryn/UI/Widget.h>

#include <functional>
#include <string>
#include <vector>

namespace alryn::ui {

// A background card with a subtle border. Pure container; add children to it.
class Panel : public Widget {
public:
    Vec4 color = theme().panel;
    Vec4 border = theme().panel_border;
    f32 radius = 14.0f;
    bool fill = true;

protected:
    void on_draw(DrawList& dl) override;
};

// Static text. `size` is the cap height in px; aligned within bounds.
class Label : public Widget {
public:
    Label() = default;
    Label(std::string label_text, f32 cap_height, TextAlign text_align = TextAlign::Left)
        : text(std::move(label_text)), size(cap_height), align(text_align) {}

    std::string text;
    f32 size = 22.0f;
    Vec4 color = theme().text;
    TextAlign align = TextAlign::Left;

protected:
    void on_draw(DrawList& dl) override;
};

// A clickable button with a hover/press animation. `primary` paints it with the
// accent colour. Set on_click for the action.
class Button : public Widget {
public:
    Button() = default;
    explicit Button(std::string text, std::function<void()> click = {})
        : label(std::move(text)), on_click(std::move(click)) {}

    std::string label;
    std::function<void()> on_click;
    bool primary = false;
    f32 text_size = 22.0f;

protected:
    void on_update(f32 dt) override;
    void on_draw(DrawList& dl) override;
    bool on_pointer_move(const Vec2& p) override;
    bool on_pointer_down(const Vec2& p, int button) override;
    bool on_pointer_up(const Vec2& p, int button) override;

private:
    bool hovered_ = false;
    bool pressed_ = false;
    f32 hover_anim_ = 0.0f;
};

// A labelled on/off pill switch. on_change fires with the new value.
class Toggle : public Widget {
public:
    Toggle() = default;
    Toggle(std::string text, bool initial, std::function<void(bool)> change = {})
        : label(std::move(text)), value(initial), on_change(std::move(change)) {}

    std::string label;
    bool value = false;
    std::function<void(bool)> on_change;
    f32 text_size = 20.0f;

protected:
    void on_update(f32 dt) override;
    void on_draw(DrawList& dl) override;
    bool on_pointer_move(const Vec2& p) override;
    bool on_pointer_down(const Vec2& p, int button) override;

private:
    Rect switch_rect() const; // the pill on the right of the row
    bool hovered_ = false;
    f32 anim_ = 0.0f; // 0 = off, 1 = on
};

// A labelled horizontal slider over [min,max]. on_change fires while dragging.
class Slider : public Widget {
public:
    Slider() = default;
    Slider(std::string text, f32 initial, f32 lo, f32 hi, std::function<void(f32)> change = {})
        : label(std::move(text)), value(initial), min_value(lo), max_value(hi),
          on_change(std::move(change)) {}

    std::string label;
    f32 value = 0.0f;
    f32 min_value = 0.0f;
    f32 max_value = 1.0f;
    bool integer = false; // snap to whole numbers
    std::function<void(f32)> on_change;
    f32 text_size = 20.0f;

protected:
    void on_draw(DrawList& dl) override;
    bool on_pointer_move(const Vec2& p) override;
    bool on_pointer_down(const Vec2& p, int button) override;
    bool on_pointer_up(const Vec2& p, int button) override;

private:
    Rect track_rect() const;
    void set_from_pointer(f32 px);
    f32 norm() const;
    bool dragging_ = false;
};

// A labelled "< value >" option cycler. on_change fires with the new index.
class Stepper : public Widget {
public:
    Stepper() = default;
    Stepper(std::string text, std::vector<std::string> opts, usize initial,
            std::function<void(usize)> change = {})
        : label(std::move(text)), options(std::move(opts)), index(initial),
          on_change(std::move(change)) {}

    std::string label;
    std::vector<std::string> options;
    usize index = 0;
    std::function<void(usize)> on_change;
    f32 text_size = 20.0f;

protected:
    void on_update(f32 dt) override;
    void on_draw(DrawList& dl) override;
    bool on_pointer_move(const Vec2& p) override;
    bool on_pointer_down(const Vec2& p, int button) override;

private:
    Rect left_arrow() const;
    Rect right_arrow() const;
    void step(int dir);
    int hovered_ = 0; // 0 none, -1 left, +1 right
};

// A horizontal row of selectable colour swatches. The selected swatch gets an
// accent ring; on_change fires with the chosen index. Used for skin/hair colour.
class SwatchRow : public Widget {
public:
    SwatchRow() = default;
    SwatchRow(std::vector<Vec3> swatches, usize selected, std::function<void(usize)> change = {})
        : colors(std::move(swatches)), index(selected), on_change(std::move(change)) {}

    std::vector<Vec3> colors;
    usize index = 0;
    std::function<void(usize)> on_change;
    f32 gap = 8.0f;

protected:
    void on_draw(DrawList& dl) override;
    bool on_pointer_move(const Vec2& p) override;
    bool on_pointer_down(const Vec2& p, int button) override;

private:
    Rect swatch_rect(usize i) const;
    int hovered_ = -1;
};

// A single-line text input. `filter` (if set) gates accepted characters.
class TextField : public Widget {
public:
    TextField() = default;
    explicit TextField(std::string initial) : text(std::move(initial)) {}

    std::string text;
    std::string placeholder;
    bool focused = false;
    usize max_length = 64;
    std::function<bool(char)> filter;     // return true to accept the char
    std::function<void(const std::string&)> on_change;
    f32 text_size = 20.0f;

protected:
    void on_update(f32 dt) override;
    void on_draw(DrawList& dl) override;
    bool on_pointer_down(const Vec2& p, int button) override;
    bool on_text(char c) override;
    bool on_key(KeyCode key) override;

private:
    f32 caret_blink_ = 0.0f;
};

} // namespace alryn::ui
