#include <Alryn/UI/UIContext.h>

#include <Alryn/UI/DrawList.h>

namespace alryn::ui {

void UIContext::set_screen(f32 width, f32 height) {
    root_.bounds = Rect{0.0f, 0.0f, width, height};
}

void UIContext::update(f32 dt, const Vec2& pointer) {
    root_.dispatch_pointer_move(pointer);
    root_.update(dt);
}

void UIContext::render(Renderer& renderer) {
    DrawList draw_list{renderer};
    root_.draw(draw_list);
}

} // namespace alryn::ui
