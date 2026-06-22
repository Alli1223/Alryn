// ClientApp - main menu / settings / customise screens and the turntable preview.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

namespace alryn::game {

void ClientApp::escape_pressed() {
    if (state_ == AppState::Menu) {
        menu_escape();
    } else if (!paused_) {
        enter_pause();
    } else if (current_screen_ == Screen::Settings) {
        show_screen(Screen::Pause);
    } else {
        resume();
    }
}

void ClientApp::rebuild_ui() {
    if (renderer_ == nullptr) {
        return;
    }
    const VkExtent2D e = renderer_->extent();
    const f32 w = static_cast<f32>(e.width);
    const f32 h = static_cast<f32>(e.height);
    ui_.set_screen(w, h);
    ui_.root().clear_children();
    if (paused_) {
        // Dim the live game behind the pause UI.
        auto& dim = ui_.root().add<ui::Panel>();
        dim.bounds = ui::Rect{0.0f, 0.0f, w, h};
        dim.color = ui::theme().overlay;
        dim.border = Vec4{0.0f};
        dim.radius = 0.0f;
    }
    switch (current_screen_) {
        case Screen::Main: build_main(w, h); break;
        case Screen::Join: build_join(w, h); break;
        case Screen::Settings: build_settings(w, h); break;
        case Screen::Customise: build_customise(w, h); break;
        case Screen::Pause: build_pause(w, h); break;
    }
}

void ClientApp::build_pause(f32 w, f32 h) {
    auto& title = ui_.root().add<ui::Label>("PAUSED", std::min(w, h) * 0.07f, ui::TextAlign::Center);
    title.bounds = ui::Rect{0.0f, h * 0.24f, w, std::min(w, h) * 0.08f};

    constexpr f32 cw = 340.0f, pad = 26.0f, rh = 52.0f, gap = 14.0f;
    constexpr int rows = 4;
    const f32 ch = pad * 2.0f + rows * rh + (rows - 1) * gap;
    const ui::Rect card{(w - cw) * 0.5f, h * 0.40f, cw, ch};
    auto& panel = ui_.root().add<ui::Panel>();
    panel.bounds = card;
    auto row = [&](int i) {
        return ui::Rect{card.x + pad, card.y + pad + static_cast<f32>(i) * (rh + gap),
                        card.w - pad * 2.0f, rh};
    };
    auto& resume = panel.add<ui::Button>("RESUME", [this] { this->resume(); });
    resume.primary = true;
    resume.bounds = row(0);
    panel.add<ui::Button>("SETTINGS", [this] { show_screen(Screen::Settings); }).bounds = row(1);
    panel.add<ui::Button>("MAIN MENU", [this] { return_to_menu(); }).bounds = row(2);
    panel.add<ui::Button>("EXIT GAME", [this] { close(); }).bounds = row(3);
}

void ClientApp::add_title(f32 w, f32 h, const char* heading, const char* sub) {
    const f32 big = std::min(w, h) * 0.11f;
    auto& title = ui_.root().add<ui::Label>(heading, big, ui::TextAlign::Center);
    title.bounds = ui::Rect{0.0f, h * 0.15f, w, big};
    if (sub != nullptr) {
        auto& s = ui_.root().add<ui::Label>(sub, 15.0f, ui::TextAlign::Center);
        s.bounds = ui::Rect{0.0f, h * 0.15f + big + 10.0f, w, 22.0f};
        s.color = ui::theme().text_muted;
    }
}

void ClientApp::build_main(f32 w, f32 h) {
    add_title(w, h, "ALRYN", "LOW - POLY  MULTIPLAYER  SANDBOX");
    constexpr f32 cw = 360.0f, pad = 26.0f, rh = 52.0f, gap = 13.0f;
    constexpr int rows = 5;
    const f32 ch = pad * 2.0f + rows * rh + (rows - 1) * gap;
    const ui::Rect card{(w - cw) * 0.5f, h * 0.40f, cw, ch};
    auto& panel = ui_.root().add<ui::Panel>();
    panel.bounds = card;
    auto row = [&](int i) {
        return ui::Rect{card.x + pad, card.y + pad + static_cast<f32>(i) * (rh + gap),
                        card.w - pad * 2.0f, rh};
    };
    auto& host = panel.add<ui::Button>("HOST GAME", [this] { enter_game(true, "127.0.0.1"); });
    host.primary = true;
    host.bounds = row(0);
    panel.add<ui::Button>("CUSTOMISE", [this] { show_screen(Screen::Customise); }).bounds = row(1);
    panel.add<ui::Button>("JOIN GAME", [this] { show_screen(Screen::Join); }).bounds = row(2);
    panel.add<ui::Button>("SETTINGS", [this] { show_screen(Screen::Settings); }).bounds = row(3);
    panel.add<ui::Button>("QUIT", [this] { close(); }).bounds = row(4);
}

void ClientApp::build_join(f32 w, f32 h) {
    add_title(w, h, "JOIN GAME", nullptr);
    constexpr f32 cw = 420.0f, pad = 26.0f, rh = 52.0f, gap = 16.0f;
    const f32 ch = pad * 2.0f + 4.0f * rh + 3.0f * gap;
    const ui::Rect card{(w - cw) * 0.5f, h * 0.40f, cw, ch};
    auto& panel = ui_.root().add<ui::Panel>();
    panel.bounds = card;
    auto row = [&](int i) {
        return ui::Rect{card.x + pad, card.y + pad + static_cast<f32>(i) * (rh + gap),
                        card.w - pad * 2.0f, rh};
    };
    auto& label = panel.add<ui::Label>("SERVER ADDRESS", 15.0f);
    label.bounds = row(0);
    label.color = ui::theme().text_muted;

    auto& field = panel.add<ui::TextField>(host_ip_);
    field.placeholder = "127.0.0.1";
    field.bounds = row(1);
    field.focused = true;
    field.filter = [](char c) {
        return (c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-' ||
               (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    field.on_change = [this](const std::string& s) { host_ip_ = s; };

    auto& connect = panel.add<ui::Button>("CONNECT", [this] {
        enter_game(false, host_ip_.empty() ? std::string{"127.0.0.1"} : host_ip_);
    });
    connect.primary = true;
    connect.bounds = row(2);
    panel.add<ui::Button>("BACK", [this] { show_screen(Screen::Main); }).bounds = row(3);
}

void ClientApp::build_settings(f32 w, f32 h) {
    add_title(w, h, "SETTINGS", nullptr);
    constexpr f32 cw = 460.0f, pad = 28.0f, rh = 54.0f, gap = 18.0f;
    const f32 ch = pad * 2.0f + 4.0f * rh + 3.0f * gap;
    const ui::Rect card{(w - cw) * 0.5f, h * 0.38f, cw, ch};
    auto& panel = ui_.root().add<ui::Panel>();
    panel.bounds = card;
    auto row = [&](int i) {
        return ui::Rect{card.x + pad, card.y + pad + static_cast<f32>(i) * (rh + gap),
                        card.w - pad * 2.0f, rh};
    };

    panel.add<ui::Toggle>("VSYNC", vsync_, [this](bool v) {
             vsync_ = v;
             if (renderer_ != nullptr) {
                 renderer_->set_vsync(v);
             }
         }).bounds = row(0);

    std::vector<std::string> res{"1280 X 720", "1600 X 900", "1920 X 1080", "FULLSCREEN"};
    panel.add<ui::Stepper>("RESOLUTION", std::move(res), res_index_,
                           [this](usize i) { apply_resolution(i); })
        .bounds = row(1);

    auto& rd = panel.add<ui::Slider>("RENDER DISTANCE", static_cast<f32>(render_distance_), 2.0f,
                                     8.0f, [this](f32 v) {
                                         render_distance_ = static_cast<int>(std::lround(v));
                                     });
    rd.integer = true;
    rd.bounds = row(2);

    panel.add<ui::Button>("BACK", [this] { settings_back(); }).bounds = row(3);
}

void ClientApp::build_customise(f32 w, f32 h) {
    rebuild_preview();

    // Controls live in a panel on the right; the 3D preview fills the rest
    // (drawn in on_render). Lay the rows out with a running vertical cursor.
    constexpr f32 pw = 400.0f;
    const ui::Rect card{w - pw - 44.0f, h * 0.12f, pw, h * 0.76f};
    auto& panel = ui_.root().add<ui::Panel>();
    panel.bounds = card;
    customise_panel_ = card;

    const f32 x = card.x + 28.0f;
    const f32 cwid = card.w - 56.0f;
    f32 y = card.y + 26.0f;
    auto place = [&](ui::Widget& widget, f32 height, f32 after = 14.0f) {
        widget.bounds = ui::Rect{x, y, cwid, height};
        y += height + after;
    };
    auto caption = [&](const char* text) {
        auto& l = panel.add<ui::Label>(text, 14.0f);
        l.bounds = ui::Rect{x, y, cwid, 16.0f};
        l.color = ui::theme().text_muted;
        y += 22.0f;
    };

    auto& header = panel.add<ui::Label>("CHARACTER", 30.0f);
    place(header, 38.0f, 18.0f);

    caption("ROLE");
    place(panel.add<ui::Stepper>(
              "ROLE", std::vector<std::string>{"KNIGHT (TANK)", "HUNTER (DMG)", "CLERIC (HEAL)"},
              static_cast<usize>(role_),
              [this](usize i) { role_ = static_cast<PlayerRole>(i % kRoleCount); }),
          46.0f);

    caption("SKIN TONE");
    place(panel.add<ui::SwatchRow>(
              std::vector<Vec3>(skin_tones().begin(), skin_tones().end()), appearance_.skin,
              [this](usize i) { appearance_.skin = static_cast<u8>(i); rebuild_preview(); }),
          40.0f);

    caption("HAIR COLOUR");
    place(panel.add<ui::SwatchRow>(
              std::vector<Vec3>(hair_colors().begin(), hair_colors().end()),
              appearance_.hair_color,
              [this](usize i) { appearance_.hair_color = static_cast<u8>(i); rebuild_preview(); }),
          40.0f);

    place(panel.add<ui::Stepper>(
              "EYES", std::vector<std::string>{"ROUND", "WIDE", "SLEEPY", "SHARP"},
              static_cast<usize>(appearance_.eyes),
              [this](usize i) { appearance_.eyes = static_cast<EyeStyle>(i); rebuild_preview(); }),
          46.0f);
    place(panel.add<ui::Stepper>(
              "EARS", std::vector<std::string>{"ROUND", "POINTED", "SMALL"},
              static_cast<usize>(appearance_.ears),
              [this](usize i) { appearance_.ears = static_cast<EarStyle>(i); rebuild_preview(); }),
          46.0f);
    place(panel.add<ui::Stepper>(
              "HAIR", std::vector<std::string>{"BALD", "SHORT", "SPIKY", "MOHAWK", "PONYTAIL"},
              static_cast<usize>(appearance_.hair),
              [this](usize i) { appearance_.hair = static_cast<HairStyle>(i); rebuild_preview(); }),
          46.0f);

    // Bottom action row: BACK + PLAY side by side.
    const f32 by = card.y + card.h - 50.0f - 24.0f;
    const f32 half = (cwid - 12.0f) * 0.5f;
    auto& back = panel.add<ui::Button>("BACK", [this] { show_screen(Screen::Main); });
    back.bounds = ui::Rect{x, by, half, 50.0f};
    auto& play = panel.add<ui::Button>("PLAY", [this] { enter_game(true, "127.0.0.1"); });
    play.primary = true;
    play.bounds = ui::Rect{x + half + 12.0f, by, half, 50.0f};
}

void ClientApp::apply_resolution(usize idx) {
    res_index_ = idx;
    if (window() == nullptr) {
        return;
    }
    static constexpr UVec2 sizes[3] = {{1280, 720}, {1600, 900}, {1920, 1080}};
    if (idx < 3) {
        window()->set_fullscreen(false);
        window()->set_size(sizes[idx].x, sizes[idx].y);
    } else {
        window()->set_fullscreen(true);
    }
    if (renderer_ != nullptr) {
        renderer_->request_resize();
    }
}

void ClientApp::draw_preview() {
    const VkExtent2D ext = renderer_->extent();
    const f32 W = static_cast<f32>(ext.width);
    const f32 H = static_cast<f32>(ext.height);
    if (W <= 0.0f || H <= 0.0f) {
        return;
    }
    const f32 aspect = W / H;
    const f32 panel_left = customise_panel_.w > 0.0f ? customise_panel_.x : W;

    const f32 fovy = radians(32.0f);
    const f32 tan_v = std::tan(fovy * 0.5f);
    const f32 height = preview_model_.height();
    const f32 ty = height * 0.5f;          // look at the avatar's middle
    const f32 half_h = height * 0.62f;     // half-height + headroom (covers hair)
    const f32 half_w = 0.55f;              // generous half-width
    const f32 free_frac = glm::clamp(panel_left / W, 0.25f, 1.0f);

    // Distance that fits both the height and the (panel-limited) width, + margin.
    const f32 dist_v = half_h / tan_v;
    const f32 dist_h = half_w / (tan_v * aspect * std::max(free_frac * 0.85f, 0.1f));
    const f32 dist = std::max(dist_v, dist_h) * 1.08f;

    // Offset the look-at so world x=0 projects to the centre of the free area.
    const f32 ndc_x = 2.0f * (panel_left * 0.5f / W) - 1.0f;
    const f32 target_x = -ndc_x * tan_v * aspect * dist;

    const Vec3 target{target_x, ty, 0.0f};
    const Vec3 eye = target + Vec3{0.0f, ty * 0.18f, dist};
    camera_.set_perspective(fovy, aspect, 0.1f, 50.0f);
    camera_.look_at(eye, target);
    renderer_->set_camera(camera_);

    const Mat4 root = glm::rotate(Mat4{1.0f}, preview_turn_, Vec3{0.0f, 1.0f, 0.0f}) *
                      preview_anim_.body_offset(); // soft idle breathe on the turntable
    const std::vector<Quat> pose = preview_anim_.pose(preview_model_);
    draw_rig(preview_model_, preview_model_.bone_matrices(root, pose));
}

} // namespace alryn::game
