// ClientApp - main menu / settings / customise screens, the turntable preview and the living
// backdrop behind the menus. (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"
#include "HudStyle.h"

#include <functional>

namespace alryn::game {

namespace {
// Magicka-style signature outfit colour per role (an index into outfit_tints): blue Knight, green
// Hunter, white Cleric, violet Mage - so the four roles read as distinct colours by default. Picking
// a role sets this as the starting outfit tint; the player can still recolour from the swatches.
u8 signature_tint(PlayerRole r) {
    switch (r) {
        case PlayerRole::Knight: return 0; // royal blue
        case PlayerRole::Hunter: return 2; // forest green
        case PlayerRole::Cleric: return 5; // white / silver
        case PlayerRole::Mage: return 3;   // arcane violet
    }
    return 0;
}

// The showcase world the menu backdrop drifts over (a fixed seed, so the menu always opens on the
// same handsome town).
constexpr u32 kMenuSeed = 4242u;

// Darkens the top of the screen (behind the title), the bottom and the corners, so the menus
// read cleanly over the bright live scene behind them.
class MenuBackdrop : public ui::Widget {
protected:
    void on_draw(ui::DrawList& dl) override {
        const f32 W = bounds.w, H = bounds.h;
        const Vec4 ink{0.03f, 0.02f, 0.012f, 0.0f};
        dl.gradient(Vec4{0.0f, 0.0f, W, H * 0.5f}, Vec4{ink.r, ink.g, ink.b, 0.78f}, ink);
        dl.gradient(Vec4{0.0f, H * 0.55f, W, H * 0.45f}, ink, Vec4{ink.r, ink.g, ink.b, 0.62f});
        dl.glow(Vec4{-W * 0.3f, -H * 0.3f, W * 1.6f, H * 1.6f}, ink, Vec4{ink.r, ink.g, ink.b, 0.85f}, 0.3f);
    }
};

// The ornament under a screen title: a gold rule fading out toward both ends, with a gilded stud
// flanked by two smaller ones at its centre.
class Ornament : public ui::Widget {
protected:
    void on_draw(ui::DrawList& dl) override {
        const ui::Theme& th = ui::theme();
        const f32 cy = bounds.y + bounds.h * 0.5f;
        const f32 cx = bounds.x + bounds.w * 0.5f;
        const f32 gap = 22.0f;
        const f32 half = bounds.w * 0.5f - gap;
        constexpr int kSeg = 16;
        for (const f32 side : {-1.0f, 1.0f}) {
            for (int i = 0; i < kSeg; ++i) {
                const f32 t0 = static_cast<f32>(i) / kSeg, t1 = static_cast<f32>(i + 1) / kSeg;
                const f32 a = (1.0f - t0) * (1.0f - t0);
                dl.line(Vec2{cx + side * (gap + half * t0), cy}, Vec2{cx + side * (gap + half * t1), cy},
                        1.5f, Vec4{th.accent_hover.r, th.accent_hover.g, th.accent_hover.b, 0.9f * a});
            }
        }
        hud::stud(dl, Vec2{cx, cy}, 4.5f);
        hud::stud(dl, Vec2{cx - 12.0f, cy}, 2.5f);
        hud::stud(dl, Vec2{cx + 12.0f, cy}, 2.5f);
    }
};

// A selectable class card: the role's crest on a gilded boss, its name in display capitals, its
// part in the party, health and weapon. The chosen card stands lifted in a warm glow; the rest lift
// a little under the cursor.
class RoleCard : public ui::Widget {
public:
    std::string name, tag, hp, weapon;
    Vec3 color{1.0f};
    bool selected = false;
    std::function<void()> on_click;
    std::function<void(ui::DrawList&, Vec2, f32)> icon; // paints the role crest at (centre, radius)

    bool can_focus() const override { return true; }
    void on_activate() override {
        if (on_click) {
            on_click();
        }
    }

protected:
    void on_update(f32 dt) override {
        const f32 target = hovered_ ? 1.0f : 0.0f;
        hover_ += (target - hover_) * std::min(1.0f, dt * 12.0f);
    }
    bool on_pointer_move(const Vec2& p) override {
        hovered_ = bounds.contains(p);
        return false;
    }
    bool on_pointer_down(const Vec2& p, int button) override {
        pressed_ = button == 0 && bounds.contains(p);
        return pressed_;
    }
    bool on_pointer_up(const Vec2& p, int button) override {
        const bool fire = pressed_ && button == 0 && bounds.contains(p);
        pressed_ = false;
        if (fire && on_click) {
            on_click();
        }
        return fire;
    }
    void on_draw(ui::DrawList& dl) override {
        const ui::Theme& th = ui::theme();
        const f32 lift = selected ? 8.0f : hover_ * 4.0f;
        const Vec4 r{bounds.x, bounds.y - lift, bounds.w, bounds.h};
        const f32 rad = 12.0f;
        if (selected) {
            dl.shadow(r, rad, 24.0f, Vec4{th.accent_hover.r, th.accent_hover.g, th.accent_hover.b, 0.45f});
        }
        dl.shadow(r, rad, 16.0f, Vec4{0.0f, 0.0f, 0.0f, 0.6f}, Vec2{0.0f, 8.0f + lift});
        dl.gradient(r, th.panel, th.panel_bottom, rad,
                    selected ? th.accent_hover : hud::alpha(th.accent, 0.45f + 0.3f * hover_),
                    selected ? 2.5f : 1.5f);
        // The role's colour glowing up behind the crest.
        const f32 cr = std::min(r.z, r.w) * 0.2f;
        const Vec2 cc{r.x + r.z * 0.5f, r.y + r.w * 0.26f};
        dl.glow(Vec4{cc.x - cr * 2.6f, cc.y - cr * 2.2f, cr * 5.2f, cr * 4.4f},
                Vec4{color, selected ? 0.42f : 0.22f + 0.1f * hover_}, Vec4{color, 0.0f});
        hud::medallion(dl, cc, cr, Vec4{0.06f, 0.045f, 0.03f, 1.0f});
        if (icon) {
            icon(dl, cc, cr * 0.48f);
        }

        f32 y = cc.y + cr + 22.0f;
        const f32 ns = glm::clamp(r.z * 0.11f, 18.0f, 28.0f);
        ui::TextStyle st = hud::style(ns, selected ? th.title : th.text, ui::TextAlign::Center,
                                      ui::FontFace::Display);
        if (selected) {
            st.color_bottom = th.title_bottom;
        }
        st.tracking = 0.06f;
        dl.text(Vec2{cc.x, y}, name, st);
        y += ns + 12.0f;
        hud::text(dl, Vec2{cc.x, y}, tag, ns * 0.55f, Vec4{glm::mix(color, Vec3{1.0f}, 0.35f), 1.0f},
                  ui::TextAlign::Center);
        y += ns * 0.55f + 16.0f;
        const f32 cs = ns * 0.48f;
        const f32 cw = hud::width(hp, cs) + cs * 1.2f;
        hud::chip(dl, Vec2{cc.x - cw * 0.5f, y}, hp, cs, hud::kGood);
        y += cs * 1.7f + 12.0f;
        hud::text(dl, Vec2{cc.x, y}, weapon, ns * 0.46f, th.text_muted, ui::TextAlign::Center);
        if (selected) {
            const f32 bs = ns * 0.45f;
            const f32 bw = hud::width("CHOSEN", bs) + bs * 1.2f;
            hud::chip(dl, Vec2{cc.x - bw * 0.5f, r.y + r.w - bs * 1.7f - 14.0f}, "CHOSEN", bs, hud::kGold);
        }
    }

private:
    bool hovered_ = false;
    bool pressed_ = false;
    f32 hover_ = 0.0f;
};
} // namespace

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
        dim.shadow = false;
        dim.ornate = false;
    }
    // A vignette so the menus read over the live scene (the dim above does it for pause).
    ui_.root().add<MenuBackdrop>().bounds = ui::Rect{0.0f, 0.0f, w, h};
    switch (current_screen_) {
        case Screen::Main: build_main(w, h); break;
        case Screen::Join: build_join(w, h); break;
        case Screen::Settings: build_settings(w, h); break;
        case Screen::Customise: build_customise(w, h); break;
        case Screen::Class: build_class(w, h); break;
        case Screen::Pause: build_pause(w, h); break;
    }
}

f32 ClientApp::add_title(f32 w, f32 h, const char* heading, const char* sub) {
    // Sized to the screen, but kept within ~80% of its width however long the heading is.
    f32 big = std::min(w, h) * 0.1f;
    ui::TextStyle probe;
    probe.face = ui::theme().title_font;
    probe.size = big;
    probe.tracking = 0.04f;
    const f32 tw = ui::DrawList::text_width(heading, probe);
    if (tw > w * 0.8f) {
        big *= w * 0.8f / tw;
    }
    f32 y = h * 0.07f;
    auto& title = ui_.root().add<ui::Label>(heading, big, ui::TextAlign::Center);
    title.heading();
    title.bounds = ui::Rect{0.0f, y, w, big};
    y += big + 14.0f;
    const f32 ow = std::min(w * 0.5f, std::max(tw, big * 4.0f));
    ui_.root().add<Ornament>().bounds = ui::Rect{(w - ow) * 0.5f, y, ow, 12.0f};
    y += 20.0f;
    if (sub != nullptr) {
        auto& s = ui_.root().add<ui::Label>(sub, 14.0f, ui::TextAlign::Center);
        s.bounds = ui::Rect{0.0f, y, w, 18.0f};
        s.color = ui::theme().text_muted;
        s.face = ui::FontFace::Bold;
        s.tracking = 0.18f;
        y += 26.0f;
    }
    return y + 10.0f;
}

void ClientApp::build_pause(f32 w, f32 h) {
    const f32 top = add_title(w, h, "PAUSED", nullptr);
    constexpr f32 cw = 340.0f, pad = 28.0f, rh = 52.0f, gap = 14.0f;
    constexpr int rows = 4;
    const f32 ch = pad * 2.0f + rows * rh + (rows - 1) * gap;
    const ui::Rect card{(w - cw) * 0.5f, std::max(top + 20.0f, (h - ch) * 0.5f), cw, ch};
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

void ClientApp::build_main(f32 w, f32 h) {
    const f32 top = add_title(w, h, "ALRYN", "A MEDIEVAL WAGON-ESCORT ADVENTURE");
    constexpr f32 cw = 360.0f, pad = 28.0f, rh = 52.0f, gap = 13.0f;
    constexpr int rows = 5;
    const f32 ch = pad * 2.0f + rows * rh + (rows - 1) * gap;
    const ui::Rect card{(w - cw) * 0.5f, std::max(top + 16.0f, h * 0.38f), cw, ch};
    auto& panel = ui_.root().add<ui::Panel>();
    panel.bounds = card;
    auto row = [&](int i) {
        return ui::Rect{card.x + pad, card.y + pad + static_cast<f32>(i) * (rh + gap),
                        card.w - pad * 2.0f, rh};
    };
    auto& host = panel.add<ui::Button>("HOST GAME", [this] {
        pending_host_local_ = true; // pick a class first, then start the listen server
        pending_host_ip_ = "127.0.0.1";
        show_screen(Screen::Class);
    });
    host.primary = true;
    host.bounds = row(0);
    panel.add<ui::Button>("CUSTOMISE", [this] { show_screen(Screen::Customise); }).bounds = row(1);
    panel.add<ui::Button>("JOIN GAME", [this] { show_screen(Screen::Join); }).bounds = row(2);
    panel.add<ui::Button>("SETTINGS", [this] { show_screen(Screen::Settings); }).bounds = row(3);
    panel.add<ui::Button>("QUIT", [this] { close(); }).bounds = row(4);

    auto& ver = ui_.root().add<ui::Label>("V0.1  -  F12 SAVES A SCREENSHOT", 11.0f, ui::TextAlign::Right);
    ver.bounds = ui::Rect{0.0f, h - 30.0f, w - 22.0f, 14.0f};
    ver.color = ui::theme().text_muted;
}

void ClientApp::build_join(f32 w, f32 h) {
    const f32 top = add_title(w, h, "JOIN GAME", "CONNECT TO A FRIEND'S HOSTED GAME");
    constexpr f32 cw = 420.0f, pad = 28.0f, rh = 52.0f, gap = 16.0f;
    const f32 ch = pad * 2.0f + 4.0f * rh + 3.0f * gap;
    const ui::Rect card{(w - cw) * 0.5f, std::max(top + 16.0f, h * 0.38f), cw, ch};
    auto& panel = ui_.root().add<ui::Panel>();
    panel.bounds = card;
    auto row = [&](int i) {
        return ui::Rect{card.x + pad, card.y + pad + static_cast<f32>(i) * (rh + gap),
                        card.w - pad * 2.0f, rh};
    };
    auto& label = panel.add<ui::Label>("SERVER ADDRESS", 15.0f);
    label.bounds = row(0);
    label.color = ui::theme().text_muted;
    label.face = ui::FontFace::Bold;
    label.tracking = 0.12f;

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
        pending_host_local_ = false; // pick a class first, then connect to the server
        pending_host_ip_ = host_ip_.empty() ? std::string{"127.0.0.1"} : host_ip_;
        show_screen(Screen::Class);
    });
    connect.primary = true;
    connect.bounds = row(2);
    panel.add<ui::Button>("BACK", [this] { show_screen(Screen::Main); }).bounds = row(3);
}

void ClientApp::build_settings(f32 w, f32 h) {
    const f32 top = add_title(w, h, "SETTINGS", nullptr);
    constexpr f32 cw = 460.0f, pad = 28.0f;
    // Rows shrink a little on a short window so the card always fits under the title.
    const f32 avail = h - top - 30.0f;
    const f32 k = glm::clamp((avail - pad * 2.0f) / (6.0f * 54.0f + 5.0f * 18.0f), 0.7f, 1.0f);
    const f32 rh = 54.0f * k, gap = 18.0f * k;
    const f32 ch = pad * 2.0f + 6.0f * rh + 5.0f * gap;
    const ui::Rect card{(w - cw) * 0.5f, top + std::max(0.0f, (avail - ch) * 0.3f), cw, ch};
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

    const bool is_fullscreen = window() != nullptr && window()->fullscreen();
    panel.add<ui::Toggle>("FULLSCREEN", is_fullscreen, [this](bool on) {
             if (window() == nullptr) {
                 return;
             }
             window()->set_fullscreen(on);
             if (renderer_ != nullptr) {
                 renderer_->request_resize();
             }
             if (Audio* a = audio()) {
                 a->play(SfxId::UiClick);
             }
         }).bounds = row(1);

    // Windowed sizes only; FULLSCREEN is the toggle above. Picking a size here
    // drops out of fullscreen (see apply_resolution).
    std::vector<std::string> res{"1280 X 720", "1600 X 900", "1920 X 1080"};
    panel.add<ui::Stepper>("RESOLUTION", std::move(res), std::min(res_index_, usize{2}),
                           [this](usize i) { apply_resolution(i); })
        .bounds = row(2);

    auto& rd = panel.add<ui::Slider>("RENDER DISTANCE", static_cast<f32>(render_distance_), 2.0f,
                                     8.0f, [this](f32 v) {
                                         render_distance_ = static_cast<int>(std::lround(v));
                                     });
    rd.integer = true;
    rd.bounds = row(3);

    // Master volume for the synthesized SFX bank; a click previews the level as you let go.
    panel.add<ui::Slider>("VOLUME", audio() != nullptr ? audio()->master_volume() : 0.8f, 0.0f,
                          1.0f,
                          [this](f32 v) {
                              if (Audio* a = audio()) {
                                  a->set_master_volume(v);
                                  a->play(SfxId::UiClick);
                              }
                          })
        .bounds = row(4);

    panel.add<ui::Button>("BACK", [this] { settings_back(); }).bounds = row(5);
}

void ClientApp::build_customise(f32 w, f32 h) {
    rebuild_preview();

    // Controls live in a panel on the right; the 3D preview fills the rest (drawn in on_render).
    // The rows are laid out with a running cursor and scaled down together on a short window so
    // everything stays inside the card.
    constexpr f32 pw = 400.0f;
    const ui::Rect card{w - pw - 44.0f, h * 0.05f, pw, h * 0.9f};
    auto& panel = ui_.root().add<ui::Panel>();
    panel.bounds = card;
    customise_panel_ = card;

    // Natural heights: header, 2 steppers + the race perk, 3 captioned swatch rows, 3 steppers,
    // the button row - with their gaps.
    constexpr f32 kStep = 42.0f, kSwatch = 34.0f, kCap = 18.0f, kGap = 10.0f;
    const f32 natural = 44.0f + 2.0f * (kStep + kGap) + (kCap + kGap) + 3.0f * (kCap + kSwatch + kGap * 1.6f) +
                        3.0f * (kStep + kGap) + 50.0f + 20.0f;
    const f32 k = glm::clamp((card.h - 48.0f) / natural, 0.62f, 1.0f);

    const f32 x = card.x + 28.0f;
    const f32 cwid = card.w - 56.0f;
    f32 y = card.y + 24.0f;
    auto place = [&](ui::Widget& widget, f32 height, f32 after) {
        widget.bounds = ui::Rect{x, y, cwid, height * k};
        y += (height + after) * k;
    };
    auto caption = [&](const char* text, const Vec4& col) {
        auto& l = panel.add<ui::Label>(text, 12.0f * std::max(k, 0.85f));
        l.bounds = ui::Rect{x, y, cwid, kCap * k};
        l.color = col;
        l.face = ui::FontFace::Bold;
        l.tracking = 0.1f;
        y += (kCap + kGap * 0.4f) * k;
    };

    auto& header = panel.add<ui::Label>("CHARACTER", 26.0f * std::max(k, 0.8f));
    header.heading();
    place(header, 32.0f, 12.0f);

    auto stepper = [&](const char* label, std::vector<std::string> opts, usize index,
                       std::function<void(usize)> change) {
        auto& s = panel.add<ui::Stepper>(label, std::move(opts), index, std::move(change));
        s.text_size = 18.0f * std::max(k, 0.8f);
        place(s, kStep, kGap);
    };
    stepper("ROLE", {"KNIGHT", "HUNTER", "CLERIC", "MAGE"}, static_cast<usize>(role_), [this](usize i) {
        role_ = static_cast<PlayerRole>(i % kRoleCount);
        equip_loadout_.outfit_tint = signature_tint(role_); // role's signature colour
        rebuild_ui(); // re-lay so the outfit-colour swatch reflects the new role
    });
    stepper("RACE", {"MAN", "DWARF", "ELF"}, static_cast<usize>(appearance_.race), [this](usize i) {
        appearance_.race = static_cast<Race>(i % kRaceCount);
        rebuild_preview(); // re-proportion the turntable avatar to the chosen race
        rebuild_ui();      // and refresh the race-perk blurb below the stepper
    });
    caption(race_perk_desc(appearance_.race), ui::theme().accent_hover); // the race's passive
    y += kGap * 0.6f * k;

    auto swatches = [&](const char* label, std::vector<Vec3> colors, usize index,
                        std::function<void(usize)> change) {
        caption(label, ui::theme().text_muted);
        place(panel.add<ui::SwatchRow>(std::move(colors), index, std::move(change)), kSwatch, kGap * 1.6f);
    };
    swatches("SKIN TONE", std::vector<Vec3>(skin_tones().begin(), skin_tones().end()), appearance_.skin,
             [this](usize i) {
                 appearance_.skin = static_cast<u8>(i);
                 rebuild_preview();
             });
    swatches("HAIR COLOUR", std::vector<Vec3>(hair_colors().begin(), hair_colors().end()),
             appearance_.hair_color, [this](usize i) {
                 appearance_.hair_color = static_cast<u8>(i);
                 rebuild_preview();
             });
    swatches("OUTFIT COLOUR", std::vector<Vec3>(outfit_tints().begin(), outfit_tints().end()),
             equip_loadout_.outfit_tint, [this](usize i) {
                 equip_loadout_.outfit_tint = static_cast<u8>(i);
                 rebuild_preview();
             });

    stepper("EYES", {"ROUND", "WIDE", "SLEEPY", "SHARP"}, static_cast<usize>(appearance_.eyes),
            [this](usize i) {
                appearance_.eyes = static_cast<EyeStyle>(i);
                rebuild_preview();
            });
    stepper("EARS", {"ROUND", "POINTED", "SMALL"}, static_cast<usize>(appearance_.ears), [this](usize i) {
        appearance_.ears = static_cast<EarStyle>(i);
        rebuild_preview();
    });
    stepper("HAIR", {"BALD", "SHORT", "SPIKY", "MOHAWK", "PONYTAIL"}, static_cast<usize>(appearance_.hair),
            [this](usize i) {
                appearance_.hair = static_cast<HairStyle>(i);
                rebuild_preview();
            });

    // Bottom action row: BACK + PLAY side by side, pinned to the card's foot.
    const f32 bh = 50.0f * std::max(k, 0.85f);
    const f32 by = card.y + card.h - bh - 24.0f;
    const f32 half = (cwid - 12.0f) * 0.5f;
    auto& back = panel.add<ui::Button>("BACK", [this] { show_screen(Screen::Main); });
    back.bounds = ui::Rect{x, by, half, bh};
    auto& play = panel.add<ui::Button>("PLAY", [this] { enter_game(true, "127.0.0.1"); });
    play.primary = true;
    play.bounds = ui::Rect{x + half + 12.0f, by, half, bh};
}

void ClientApp::build_class(f32 w, f32 h) {
    const f32 top = add_title(w, h, "CHOOSE YOUR CLASS",
                              pending_host_local_ ? "HOSTING A NEW GAME" : "JOINING A GAME");

    // A row of class cards; the chosen one stands lifted in a warm glow.
    static const char* tags[kRoleCount] = {"TANK", "RANGED DAMAGE", "HEALER", "ELEMENTAL DAMAGE"};
    static const char* hints[kRoleCount] = {"SWORD + SHIELD", "LONGBOW", "HOLY STAFF", "COMBO SPELLS"};
    const f32 gap = 20.0f;
    const f32 total_w = std::min(w * 0.9f, 1000.0f);
    const f32 card_w = (total_w - gap * static_cast<f32>(kRoleCount - 1)) / static_cast<f32>(kRoleCount);
    const f32 bottom_reserve = 130.0f; // the blurb + BACK / START under the cards
    const f32 card_h = glm::clamp(h - top - bottom_reserve - 24.0f, 220.0f, 330.0f);
    const f32 cy = top + 24.0f;
    for (int i = 0; i < kRoleCount; ++i) {
        const auto role = static_cast<PlayerRole>(i);
        auto& card = ui_.root().add<RoleCard>();
        card.bounds = ui::Rect{(w - total_w) * 0.5f + static_cast<f32>(i) * (card_w + gap), cy, card_w, card_h};
        card.name = role_name(role);
        card.tag = tags[i];
        card.hp = std::format("{} HP", static_cast<int>(role_stats(role).max_health));
        card.weapon = hints[i];
        card.color = role_color(role);
        card.selected = static_cast<int>(role_) == i;
        card.on_click = [this, i] {
            role_ = static_cast<PlayerRole>(i);
            equip_loadout_.outfit_tint = signature_tint(role_); // role's signature colour
            rebuild_ui(); // re-lay to highlight the new selection
        };
        card.icon = [this, role](ui::DrawList& dl, Vec2 c, f32 r) {
            draw_ability_icon(dl, role, 0, c.x, c.y, r, Vec4{glm::mix(role_color(role), Vec3{1.0f}, 0.15f), 1.0f});
        };
    }

    // The chosen class's fantasy, centred under the cards.
    auto& blurb = ui_.root().add<ui::Label>(role_desc(role_), 17.0f, ui::TextAlign::Center);
    blurb.bounds = ui::Rect{0.0f, cy + card_h + 22.0f, w, 22.0f};
    blurb.color = ui::theme().text;

    // BACK + START.
    const f32 bw = 200.0f, bh = 52.0f, bgap = 16.0f;
    const f32 by = cy + card_h + 62.0f;
    auto& back = ui_.root().add<ui::Button>("BACK", [this] { show_screen(Screen::Main); });
    back.bounds = ui::Rect{(w - bw * 2.0f - bgap) * 0.5f, by, bw, bh};
    auto& start = ui_.root().add<ui::Button>(pending_host_local_ ? "START" : "JOIN",
                                             [this] { enter_game(pending_host_local_, pending_host_ip_); });
    start.primary = true;
    start.bounds = ui::Rect{(w - bw * 2.0f - bgap) * 0.5f + bw + bgap, by, bw, bh};
}

void ClientApp::apply_resolution(usize idx) {
    static constexpr UVec2 sizes[3] = {{1280, 720}, {1600, 900}, {1920, 1080}};
    res_index_ = std::min(idx, usize{2});
    if (window() == nullptr) {
        return;
    }
    // Choosing a windowed size drops out of fullscreen (the FULLSCREEN toggle owns
    // that state). set_fullscreen(false) first so set_size isn't ignored.
    window()->set_fullscreen(false);
    window()->set_size(sizes[res_index_].x, sizes[res_index_].y);
    if (renderer_ != nullptr) {
        renderer_->request_resize();
    }
}

void ClientApp::rebuild_preview() {
    // The old turntable meshes may still be in flight - retire them rather than freeing now.
    retire_mesh(std::move(preview_.body_mesh));
    retire_mesh(std::move(preview_.outfit_mesh));
    // Show the role's full (master) outfit in the chosen colour, so the turntable previews the class +
    // the colour pick (in-game you start ragged and buy up to this).
    Equipment eq = equip_loadout_;
    eq.outfit_tier = 3;
    eq.weapon_tier = 3;
    const OutfitKind kind = outfit_kind_for_role(static_cast<u8>(role_));
    preview_.model = CharacterModel::create(kPreviewSeed, appearance_);
    apply_outfit(preview_.model, kind, eq);
    preview_.appearance = appearance_;
    preview_.equipment = eq;
    preview_.role = static_cast<u8>(role_);
    preview_.body_skin = build_body_mesh(preview_.model);
    preview_.outfit_skin = build_outfit_mesh(preview_.model, kind, eq);
    setup_cloth(preview_, role_, eq); // the cape / robe swings as the turntable turns
}

void ClientApp::draw_preview() {
    if (preview_.body_skin.vertices.empty()) {
        rebuild_preview();
    }
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
    const f32 height = preview_.model.height();
    const f32 ty = height * 0.52f;         // look just above the avatar's middle
    const f32 half_h = height * 0.78f;     // half-height + headroom (helms, plumes, a raised weapon)
    const f32 half_w = 0.6f;               // generous half-width (shield / bow)
    const f32 free_frac = glm::clamp(panel_left / W, 0.25f, 1.0f);

    // Distance that fits both the height and the (panel-limited) width, + margin.
    const f32 dist_v = half_h / tan_v;
    const f32 dist_h = half_w / (tan_v * aspect * std::max(free_frac * 0.85f, 0.1f));
    const f32 dist = std::max(dist_v, dist_h) * 1.12f;

    // Offset the look-at so world x=0 projects to the centre of the free area.
    const f32 ndc_x = 2.0f * (panel_left * 0.5f / W) - 1.0f;
    const f32 target_x = -ndc_x * tan_v * aspect * dist;

    const Vec3 target{target_x, ty, 0.0f};
    const Vec3 eye = target + Vec3{0.0f, ty * 0.35f, dist};
    camera_.set_perspective(fovy, aspect, 0.1f, 50.0f);
    camera_.look_at(eye, target);
    renderer_->set_camera(camera_);

    // A round stone plinth with a gold-bound rim for the avatar to stand on.
    renderer_->draw(shape_cylinder_,
                    glm::translate(Mat4{1.0f}, Vec3{0.0f, -0.08f, 0.0f}) * glm::scale(Mat4{1.0f}, Vec3{1.25f, 0.16f, 1.25f}),
                    Vec4{0.36f, 0.33f, 0.30f, 1.0f}); // top face at the avatar's feet (y = 0)
    renderer_->draw(shape_cylinder_,
                    glm::translate(Mat4{1.0f}, Vec3{0.0f, -0.035f, 0.0f}) * glm::scale(Mat4{1.0f}, Vec3{1.31f, 0.05f, 1.31f}),
                    Vec4{0.80f, 0.60f, 0.28f, 1.0f});

    const Mat4 root = glm::rotate(Mat4{1.0f}, preview_turn_, Vec3{0.0f, 1.0f, 0.0f}) *
                      preview_anim_.body_offset(); // soft idle breathe on the turntable
    // Drawn exactly as in game: the skinned body + outfit, the attachment pieces, the simulated cloth,
    // standing in the role's resting stance.
    std::vector<Quat> pose = preview_anim_.pose(preview_.model);
    apply_idle_stance(preview_.model, pose, role_, 1.0f);
    draw_skinned_body(preview_, root, pose);
    draw_rig(preview_.model, preview_.model.bone_matrices(root, pose), Vec3{1.0f}, /*attachments_only=*/true);
    const std::vector<Mat4> jmats = preview_.model.joint_matrices(root, pose);
    draw_cloth(preview_, root, jmats, Vec3{1.0f}, 0.0f);
    // Show the role's weapon in hand on the turntable too (master tier, the chosen colour).
    Equipment preview_eq = equip_loadout_;
    preview_eq.outfit_tier = 3;
    preview_eq.weapon_tier = 3;
    if (role_ == PlayerRole::Mage || role_ == PlayerRole::Cleric) {
        draw_planted_weapon(preview_.model, jmats, Vec3{0.0f}, role_, preview_eq); // resting on the staff
    } else {
        draw_role_weapon(preview_.model, jmats, role_, preview_eq);
    }
}

// ---- The living backdrop behind the menus ---------------------------------------------------

void ClientApp::update_menu_scene(Timestep dt) {
    if (renderer_ == nullptr) {
        return;
    }
    if (!menu_terrain_) {
        // The showcase town: the first one found spiralling out from the world origin.
        std::optional<worldgen::Village> town;
        for (int r = 0; r <= 6 && !town; ++r) {
            for (int vz = -r; vz <= r && !town; ++vz) {
                for (int vx = -r; vx <= r && !town; ++vx) {
                    if (std::max(std::abs(vx), std::abs(vz)) == r) {
                        town = worldgen::village_at(vx, vz, kMenuSeed);
                    }
                }
            }
        }
        const Vec2 c = town ? town->center : Vec2{0.0f};
        menu_focus_ = Vec3{c.x, worldgen::height(c.x, c.y, kMenuSeed), c.y};
        menu_terrain_ = std::make_unique<StreamingTerrain>(kMenuSeed, 0.5f, 16, 5);
        menu_cam_t_ = 0.0f;
    }
    world_seed_ = kMenuSeed; // props + lighting look up the town layout by the world seed
    menu_cam_t_ += dt.seconds;

    // Warm morning light, held still (no snapshot -> the clock only moves with dt, so pass none),
    // under a soft blue sky with a pale haze: the fog colour is also the sky's horizon band, so
    // the edge of the streamed slice of world melts into the horizon instead of ending.
    time_of_day_ = 0.30f;
    update_day_night(Timestep{0.0f});
    renderer_->set_sky_color(Vec3{0.38f, 0.54f, 0.76f});
    renderer_->set_fog(Vec3{0.74f, 0.72f, 0.68f}, 0.012f, 0.0f, 0.0f);
    // The haze (and the foliage peek-through) are measured from the "player": centre them on the
    // town, or the whole backdrop sits deep in fog relative to a player who isn't there.
    renderer_->set_player_position(menu_focus_);

    // A slow orbit over the market, looking down across the stalls and the rooftops beyond.
    const f32 a = 0.7f + menu_cam_t_ * 0.03f;
    constexpr f32 R = 22.0f;
    const Vec3 eye = menu_focus_ + Vec3{std::cos(a) * R, 16.0f, std::sin(a) * R};
    const Vec3 target = menu_focus_ + Vec3{0.0f, 1.0f, 0.0f};
    camera_.set_perspective(radians(40.0f), renderer_->aspect(), cam::near_plane, cam::far_plane);
    camera_.look_at(eye, target);
    menu_terrain_->update(menu_focus_, renderer_->device());
}

void ClientApp::draw_menu_scene() {
    if (!menu_terrain_) {
        return;
    }
    renderer_->set_camera(camera_);
    menu_terrain_->for_each_mesh([&](const Mesh& mesh) { renderer_->draw(mesh, Mat4{1.0f}); });
    menu_terrain_->for_each_vegetation_mesh(
        [&](const Mesh& mesh) { renderer_->draw_vegetation(mesh, Mat4{1.0f}); });
    menu_terrain_->for_each_tree([&](const TreeInstance& t) {
        renderer_->draw_cutout(tree_library_[tree_index(t)].trunk, tree_model(t));
        renderer_->draw_transparent(tree_library_[tree_index(t)].foliage, tree_model(t), Vec4{t.tint, 1.0f});
    });
    menu_terrain_->for_each_prop([&](const PropInstance& p) { draw_prop(p); });
    renderer_->draw_water(water_mesh_,
                          glm::translate(Mat4{1.0f}, Vec3{menu_focus_.x, worldgen::water_level, menu_focus_.z}));
}

} // namespace alryn::game
