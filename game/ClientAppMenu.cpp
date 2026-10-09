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
    std::string name, tag, hp, weapon, starts;
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
        y += ns * 0.46f + 14.0f;
        if (!starts.empty()) {
            hud::text(dl, Vec2{cc.x, y}, "STARTING SKILLS", ns * 0.38f, hud::alpha(th.accent_hover, 0.85f),
                      ui::TextAlign::Center);
            hud::text(dl, Vec2{cc.x, y + ns * 0.38f + 6.0f}, starts, ns * 0.42f, th.text, ui::TextAlign::Center);
        }
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

// One saved hero on the hero-select screen: an identity-colour stripe, the class crest, the name in
// display capitals, "LEVEL n CLASS", their place on the journey and an XP gauge. The chosen card is
// lit gold; the "forge a new hero" card is a dim, dashed invitation.
class HeroCard : public ui::Widget {
public:
    std::string name, sub, note;
    Vec3 color{1.0f};    // identity colour
    Vec3 accent{1.0f};   // the class colour (crest glow)
    f32 xp_frac = 0.0f;
    bool selected = false;
    bool is_new = false; // the "forge a new hero" card
    std::function<void()> on_click;
    std::function<void(ui::DrawList&, Vec2, f32)> icon;

    bool can_focus() const override { return true; }
    void on_activate() override {
        if (on_click) {
            on_click();
        }
    }

protected:
    void on_update(f32 dt) override {
        hover_ += ((hovered_ ? 1.0f : 0.0f) - hover_) * std::min(1.0f, dt * 12.0f);
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
            on_click(); // may rebuild the menu (destroying this card) - touch nothing after it
        }
        return fire;
    }
    void on_draw(ui::DrawList& dl) override {
        const ui::Theme& th = ui::theme();
        const Vec4 r{bounds.x - hover_ * 3.0f, bounds.y, bounds.w, bounds.h};
        const f32 rad = 10.0f;
        if (selected) {
            dl.shadow(r, rad, 18.0f, hud::alpha(th.accent_hover, 0.4f));
        }
        dl.shadow(r, rad, 10.0f, Vec4{0.0f, 0.0f, 0.0f, 0.5f}, Vec2{0.0f, 5.0f});
        const f32 a = is_new ? 0.55f + 0.25f * hover_ : 1.0f;
        dl.gradient(r, hud::alpha(th.panel, a), hud::alpha(th.panel_bottom, a), rad,
                    selected ? th.accent_hover : hud::alpha(th.accent, 0.4f + 0.35f * hover_),
                    selected ? 2.25f : 1.25f);
        const f32 cr = bounds.h * 0.30f; // crest radius
        const Vec2 cc{r.x + 18.0f + cr, r.y + bounds.h * 0.5f};
        if (is_new) {
            // A plus inside an empty medallion: "forge a new hero".
            hud::medallion(dl, cc, cr, Vec4{0.05f, 0.035f, 0.025f, 1.0f});
            const Vec4 g = hud::alpha(th.accent_hover, 0.75f + 0.25f * hover_);
            dl.line(Vec2{cc.x - cr * 0.45f, cc.y}, Vec2{cc.x + cr * 0.45f, cc.y}, 3.0f, g);
            dl.line(Vec2{cc.x, cc.y - cr * 0.45f}, Vec2{cc.x, cc.y + cr * 0.45f}, 3.0f, g);
            const f32 tx = cc.x + cr + 18.0f;
            hud::text(dl, Vec2{tx, r.y + bounds.h * 0.5f - 12.0f}, name, 17.0f, th.title, ui::TextAlign::Left,
                      ui::FontFace::Display);
            hud::text(dl, Vec2{tx, r.y + bounds.h * 0.5f + 9.0f}, sub, 11.0f, th.text_muted);
            return;
        }
        // The identity-colour stripe down the left edge.
        dl.rect(Vec4{r.x + 3.0f, r.y + 6.0f, 5.0f, bounds.h - 12.0f}, Vec4{color, 0.95f}, 2.5f);
        dl.glow(Vec4{cc.x - cr * 1.8f, cc.y - cr * 1.8f, cr * 3.6f, cr * 3.6f}, Vec4{accent, 0.18f + 0.1f * hover_},
                Vec4{accent, 0.0f});
        hud::medallion(dl, cc, cr, Vec4{0.06f, 0.045f, 0.03f, 1.0f});
        if (icon) {
            icon(dl, cc, cr * 0.5f);
        }
        const f32 tx = cc.x + cr + 16.0f;
        const f32 ns = std::clamp(bounds.h * 0.22f, 13.0f, 20.0f);
        f32 y = r.y + bounds.h * 0.16f;
        ui::TextStyle st = hud::style(ns, Vec4{glm::mix(color, Vec3{1.0f}, 0.55f), 1.0f}, ui::TextAlign::Left,
                                      ui::FontFace::Display);
        st.tracking = 0.05f;
        dl.text(Vec2{tx, y}, name, st);
        y += ns + 6.0f;
        hud::text(dl, Vec2{tx, y}, sub, ns * 0.62f, th.text);
        y += ns * 0.62f + 6.0f;
        hud::text(dl, Vec2{tx, y}, note, ns * 0.55f, th.text_muted);
        // XP gauge along the bottom.
        const Vec4 bar{tx, r.y + bounds.h - 13.0f, r.x + r.z - tx - 16.0f, 5.0f};
        dl.rect(bar, Vec4{0.02f, 0.015f, 0.01f, 0.8f}, 2.5f);
        if (xp_frac > 0.0f) {
            dl.gradient(Vec4{bar.x, bar.y, std::max(bar.z * xp_frac, 4.0f), bar.w}, hud::kGold,
                        hud::shade(hud::kGold, 0.65f), 2.5f);
        }
        if (selected) {
            const f32 bs = 10.0f;
            const f32 bw = hud::width("CHOSEN", bs) + bs * 1.2f;
            hud::chip(dl, Vec2{r.x + r.z - bw - 12.0f, r.y + 10.0f}, "CHOSEN", bs, hud::kGold);
        }
    }

private:
    bool hovered_ = false;
    bool pressed_ = false;
    f32 hover_ = 0.0f;
};

// A default name for a freshly forged hero (so nobody walks the roads as "PLAYER 3").
const char* default_hero_name(PlayerRole role, usize n) {
    static const char* names[kRoleCount][4] = {
        {"ALDRIC", "BRENNA", "GAWAIN", "ISOLDE"},  // Knight
        {"WREN", "FENWICK", "SAREI", "HOLT"},      // Hunter
        {"ELOWEN", "BEDE", "MAREN", "ANSELM"},     // Cleric
        {"MORGRA", "THESSALY", "CORVIN", "NYX"},   // Mage
    };
    return names[static_cast<u8>(role) % kRoleCount][n % 4];
}
} // namespace

void ClientApp::run_ui_script() {
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* s = std::getenv("ALRYN_UI_SCRIPT"); s != nullptr) {
            std::string all{s};
            usize start = 0;
            while (start <= all.size()) {
                const usize end = all.find(';', start);
                const std::string step = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!step.empty()) {
                    ui_script_.push_back(step);
                }
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1;
            }
        }
    }
    if (ui_script_step_ >= ui_script_.size()) {
        return;
    }
    if (--ui_script_wait_ > 0) {
        return;
    }
    ui_script_wait_ = 20;
    const std::string step = ui_script_[ui_script_step_++];
    const usize colon = step.find(':');
    const std::string verb = step.substr(0, colon);
    const std::string arg = colon == std::string::npos ? std::string{} : step.substr(colon + 1);
    ALRYN_INFO("UI script: {}", step);
    if (verb == "wait") {
        ui_script_wait_ = std::max(1, std::atoi(arg.c_str()));
    } else if (verb == "type") {
        for (const char c : arg) {
            ui_.text(c);
        }
    } else if (verb == "key") {
        // A key press through the app's normal event routing (e.g. "key:76" = L, the lantern).
        KeyPressedEvent press{static_cast<KeyCode>(std::atoi(arg.c_str()))};
        on_event(press);
    } else if (verb == "click") {
        // Find the widget by its visible label (buttons, hero cards, class cards) and click its centre
        // through the normal pointer dispatch.
        std::function<ui::Widget*(ui::Widget&)> find = [&](ui::Widget& w) -> ui::Widget* {
            for (auto it = w.children().rbegin(); it != w.children().rend(); ++it) {
                if (ui::Widget* hit = find(**it)) {
                    return hit;
                }
            }
            if (auto* b = dynamic_cast<ui::Button*>(&w); b != nullptr && b->label == arg) {
                return b;
            }
            if (auto* h = dynamic_cast<HeroCard*>(&w); h != nullptr && h->name == arg) {
                return h;
            }
            if (auto* r = dynamic_cast<RoleCard*>(&w); r != nullptr && r->name == arg) {
                return r;
            }
            return nullptr;
        };
        if (ui::Widget* target = find(ui_.root())) {
            const Vec2 c = target->bounds.center();
            ui_.pointer_down(c, 0);
            ui_.pointer_up(c, 0);
        } else {
            ALRYN_WARN("UI script: no widget labelled '{}'", arg);
        }
    }
}

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
        case Screen::Heroes: build_heroes(w, h); break;
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
    constexpr f32 cw = 380.0f, pad = 28.0f, rh = 52.0f, gap = 13.0f;
    const bool has_hero = !roster_.heroes.empty();
    const int rows = has_hero ? 5 : 4;
    const f32 ch = pad * 2.0f + static_cast<f32>(rows) * rh + static_cast<f32>(rows - 1) * gap;
    const ui::Rect card{(w - cw) * 0.5f, std::max(top + 34.0f, h * 0.38f), cw, ch};
    auto& panel = ui_.root().add<ui::Panel>();
    panel.bounds = card;
    auto row = [&](int i) {
        return ui::Rect{card.x + pad, card.y + pad + static_cast<f32>(i) * (rh + gap),
                        card.w - pad * 2.0f, rh};
    };
    int r = 0;
    if (has_hero) {
        // CONTINUE: straight back on the road with the last hero (hosting) - one click to play.
        const Hero* h0 = roster_.current();
        auto& cont = panel.add<ui::Button>("CONTINUE", [this] {
            select_hero(roster_.selected);
            enter_game(true, "127.0.0.1");
        });
        cont.primary = true;
        cont.bounds = row(r++);
        // Who CONTINUE plays as, just above the card.
        auto& who = ui_.root().add<ui::Label>(
            std::format("{}  -  LEVEL {} {}", h0->name, h0->level(), role_name(h0->role)), 14.0f, ui::TextAlign::Center);
        who.bounds = ui::Rect{0.0f, card.y - 24.0f, w, 18.0f};
        who.color = Vec4{glm::mix(player_color(h0->color), Vec3{1.0f}, 0.4f), 1.0f};
        who.face = ui::FontFace::Bold;
        who.tracking = 0.12f;
        panel.add<ui::Button>("HEROES", [this] { show_screen(Screen::Heroes); }).bounds = row(r++);
    } else {
        // No heroes yet: the journey begins by forging one.
        auto& forge = panel.add<ui::Button>("FORGE A HERO", [this] { begin_new_hero(); });
        forge.primary = true;
        forge.bounds = row(r++);
    }
    panel.add<ui::Button>("JOIN A FRIEND", [this] {
             if (roster_.heroes.empty()) {
                 begin_new_hero(); // you need a hero before you can ride with friends
             } else {
                 select_hero(roster_.selected);
                 show_screen(Screen::Join);
             }
         }).bounds = row(r++);
    panel.add<ui::Button>("SETTINGS", [this] { show_screen(Screen::Settings); }).bounds = row(r++);
    panel.add<ui::Button>("QUIT", [this] { close(); }).bounds = row(r++);

    auto& ver = ui_.root().add<ui::Label>("V0.2  -  F12 SAVES A SCREENSHOT", 11.0f, ui::TextAlign::Right);
    ver.bounds = ui::Rect{0.0f, h - 30.0f, w - 22.0f, 14.0f};
    ver.color = ui::theme().text_muted;
}

void ClientApp::build_join(f32 w, f32 h) {
    const std::string sub = std::format("RIDING AS {} - CONNECT TO A FRIEND'S GAME", hero_.name);
    const f32 top = add_title(w, h, "JOIN A FRIEND", sub.c_str());
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
        // The hero was chosen on the way here (Heroes / Main) - ride straight in.
        enter_game(false, host_ip_.empty() ? std::string{"127.0.0.1"} : host_ip_);
    });
    connect.primary = true;
    connect.bounds = row(2);
    panel.add<ui::Button>("BACK", [this] { show_screen(Screen::Heroes); }).bounds = row(3);
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

    // Step 2 of forging a hero (or editing one): the controls live in a panel on the right; the 3D
    // preview fills the rest (drawn in on_render). The rows are laid out with a running cursor and
    // scaled down together on a short window so everything stays inside the card.
    constexpr f32 pw = 400.0f;
    const ui::Rect card{w - pw - 44.0f, h * 0.04f, pw, h * 0.92f};
    auto& panel = ui_.root().add<ui::Panel>();
    panel.bounds = card;
    customise_panel_ = card;

    // Natural heights: header + class line, the name field, the race stepper + perk, 4 captioned
    // swatch rows, 3 steppers, the button row - with their gaps.
    constexpr f32 kStep = 42.0f, kSwatch = 32.0f, kCap = 18.0f, kGap = 10.0f;
    const f32 natural = 44.0f + 22.0f + (kCap + 40.0f + kGap) + (kStep + kGap) + (kCap + kGap) +
                        4.0f * (kCap + kSwatch + kGap * 1.4f) + 3.0f * (kStep + kGap) + 50.0f + 20.0f;
    const f32 k = glm::clamp((card.h - 48.0f) / natural, 0.58f, 1.0f);

    const f32 x = card.x + 28.0f;
    const f32 cwid = card.w - 56.0f;
    f32 y = card.y + 22.0f;
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

    auto& header = panel.add<ui::Label>(creating_ ? "NAME & LOOK" : "EDIT HERO", 26.0f * std::max(k, 0.8f));
    header.heading();
    place(header, 32.0f, 4.0f);
    // The class is fixed once forged (progress belongs to it) - shown, not stepped.
    auto& cls = panel.add<ui::Label>(std::format("{}  -  {}", role_name(role_), creating_ ? "STEP 2 OF 2" : "CLASS IS SET"),
                                     13.0f * std::max(k, 0.85f));
    cls.color = Vec4{glm::mix(role_color(role_), Vec3{1.0f}, 0.3f), 1.0f};
    cls.face = ui::FontFace::Bold;
    cls.tracking = 0.12f;
    place(cls, 18.0f, 10.0f);

    caption("NAME", ui::theme().text_muted);
    auto& name = panel.add<ui::TextField>(hero_.name);
    name.placeholder = default_hero_name(role_, roster_.heroes.size());
    name.max_length = kMaxNameLength;
    name.text_size = 18.0f * std::max(k, 0.8f);
    name.filter = [](char c) { return name_char_ok(c); };
    name.on_change = [this](const std::string& v) { hero_.name = v; };
    name.focused = creating_ && hero_.name.empty();
    place(name, 40.0f, kGap);

    auto stepper = [&](const char* label, std::vector<std::string> opts, usize index,
                       std::function<void(usize)> change) {
        auto& st = panel.add<ui::Stepper>(label, std::move(opts), index, std::move(change));
        st.text_size = 18.0f * std::max(k, 0.8f);
        place(st, kStep, kGap);
    };
    stepper("RACE", {"MAN", "DWARF", "ELF"}, static_cast<usize>(appearance_.race), [this](usize i) {
        appearance_.race = static_cast<Race>(i % kRaceCount);
        rebuild_ui(); // re-proportion the turntable avatar + refresh the race-perk blurb
    });
    caption(race_perk_desc(appearance_.race), ui::theme().accent_hover); // the race's passive
    y += kGap * 0.6f * k;

    auto swatches = [&](const char* label, std::vector<Vec3> colors, usize index,
                        std::function<void(usize)> change) {
        caption(label, ui::theme().text_muted);
        place(panel.add<ui::SwatchRow>(std::move(colors), index, std::move(change)), kSwatch, kGap * 1.4f);
    };
    swatches("PLAYER COLOUR  (RING + NAME)",
             std::vector<Vec3>(player_colors().begin(), player_colors().end()), hero_.color,
             [this](usize i) { hero_.color = static_cast<u8>(i); });
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

    // Bottom action row: BACK + CREATE / SAVE side by side, pinned to the card's foot.
    const f32 bh = 50.0f * std::max(k, 0.85f);
    const f32 by = card.y + card.h - bh - 22.0f;
    const f32 half = (cwid - 12.0f) * 0.5f;
    auto& back = panel.add<ui::Button>("BACK", [this] {
        if (creating_) {
            show_screen(Screen::Class);
        } else {
            select_hero(hero_index_); // discard the edits
            show_screen(Screen::Heroes);
        }
    });
    back.bounds = ui::Rect{x, by, half, bh};
    auto& done = panel.add<ui::Button>(creating_ ? "CREATE" : "SAVE", [this] {
        commit_hero();
        creating_ = false;
        show_screen(Screen::Heroes);
    });
    done.primary = true;
    done.bounds = ui::Rect{x + half + 12.0f, by, half, bh};
}

void ClientApp::build_class(f32 w, f32 h) {
    creating_ = true; // the class screen is step 1 of forging a hero
    const f32 top = add_title(w, h, "FORGE A HERO", "STEP 1 OF 2  -  CHOOSE A CLASS");

    // A row of class cards; the chosen one stands lifted in a warm glow. Each lists the two skills
    // the class starts with - the rest of the kit is earned in the skill tree as the hero levels.
    static const char* tags[kRoleCount] = {"TANK", "RANGED DAMAGE", "HEALER", "ELEMENTAL DAMAGE"};
    static const char* hints[kRoleCount] = {"SWORD + SHIELD", "LONGBOW", "HOLY STAFF", "COMBO SPELLS"};
    const f32 gap = 20.0f;
    const f32 total_w = std::min(w * 0.9f, 1000.0f);
    const f32 card_w = (total_w - gap * static_cast<f32>(kRoleCount - 1)) / static_cast<f32>(kRoleCount);
    const f32 bottom_reserve = 130.0f; // the blurb + BACK / NEXT under the cards
    const f32 card_h = glm::clamp(h - top - bottom_reserve - 24.0f, 240.0f, 370.0f);
    const f32 cy = top + 24.0f;
    for (int i = 0; i < kRoleCount; ++i) {
        const auto role = static_cast<PlayerRole>(i);
        auto& card = ui_.root().add<RoleCard>();
        card.bounds = ui::Rect{(w - total_w) * 0.5f + static_cast<f32>(i) * (card_w + gap), cy, card_w, card_h};
        card.name = role_name(role);
        card.tag = tags[i];
        card.hp = std::format("{} HP", static_cast<int>(role_stats(role).max_health));
        card.weapon = hints[i];
        std::string starts;
        for (u8 a = 0; a < kAbilityCount; ++a) {
            if (skill_node(role, a).tier == 0) {
                starts += starts.empty() ? "" : " + ";
                starts += ability_def(role, a).name;
            }
        }
        card.starts = starts;
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

    // BACK + NEXT.
    const f32 bw = 200.0f, bh = 52.0f, bgap = 16.0f;
    const f32 by = cy + card_h + 62.0f;
    auto& back = ui_.root().add<ui::Button>("BACK", [this] {
        creating_ = false;
        if (!roster_.heroes.empty()) {
            select_hero(roster_.selected);
        }
        show_screen(roster_.heroes.empty() ? Screen::Main : Screen::Heroes);
    });
    back.bounds = ui::Rect{(w - bw * 2.0f - bgap) * 0.5f, by, bw, bh};
    auto& next = ui_.root().add<ui::Button>("NEXT", [this] {
        hero_.role = role_;
        show_screen(Screen::Customise); // step 2: name + look
    });
    next.primary = true;
    next.bounds = ui::Rect{(w - bw * 2.0f - bgap) * 0.5f + bw + bgap, by, bw, bh};
}

void ClientApp::build_heroes(f32 w, f32 h) {
    confirm_delete_ = confirm_delete_ && !roster_.heroes.empty();
    if (!roster_.heroes.empty() && hero_index_ < 0) {
        select_hero(roster_.selected);
    }
    rebuild_preview();

    // The roster lives in a panel on the right (the chosen hero stands on the turntable to its left).
    constexpr f32 pw = 430.0f;
    const ui::Rect card{w - pw - 44.0f, h * 0.05f, pw, h * 0.9f};
    auto& panel = ui_.root().add<ui::Panel>();
    panel.bounds = card;
    customise_panel_ = card;
    const f32 x = card.x + 22.0f;
    const f32 cwid = card.w - 44.0f;
    auto& head = panel.add<ui::Label>("HEROES", 24.0f);
    head.heading();
    head.bounds = ui::Rect{x, card.y + 20.0f, cwid, 30.0f};
    // The screen title sits over the turntable, left of the roster panel.
    {
        const f32 big = std::clamp(card.x * 0.085f, 30.0f, 60.0f);
        auto& title = ui_.root().add<ui::Label>("CHOOSE YOUR HERO", big, ui::TextAlign::Center);
        title.heading();
        title.bounds = ui::Rect{0.0f, h * 0.06f, card.x, big};
        const f32 ow = std::min(card.x * 0.6f, big * 7.0f);
        ui_.root().add<Ornament>().bounds = ui::Rect{(card.x - ow) * 0.5f, h * 0.06f + big + 12.0f, ow, 12.0f};
    }

    // Buttons pinned to the foot: JOIN + HOST, EDIT + DELETE, BACK.
    const f32 bh = 46.0f, bgap = 10.0f;
    const f32 half = (cwid - bgap) * 0.5f;
    const f32 foot = card.y + card.h - 22.0f;
    const f32 row3 = foot - bh;
    const f32 row2 = row3 - bgap - bh;
    const f32 row1 = row2 - bgap - bh;
    const bool any = !roster_.heroes.empty();

    // The hero cards, sized to share the space above the buttons.
    const usize n = roster_.heroes.size();
    const bool can_add = n < kMaxHeroes;
    const usize slots = n + (can_add ? 1u : 0u);
    const f32 list_top = card.y + 62.0f;
    const f32 list_bot = row1 - 16.0f;
    const f32 cgap = 8.0f;
    const f32 ch = std::min(86.0f, (list_bot - list_top - cgap * static_cast<f32>(slots - 1)) / static_cast<f32>(std::max<usize>(slots, 1)));
    f32 y = list_top;
    for (usize i = 0; i < n; ++i) {
        const Hero& hh = roster_.heroes[i];
        auto& hc = panel.add<HeroCard>();
        hc.bounds = ui::Rect{x, y, cwid, ch};
        hc.name = hh.name;
        hc.sub = std::format("LEVEL {}  {}  -  {}", hh.level(), role_name(hh.role), race_name(hh.appearance.race));
        const u8 step = std::min<u8>(hh.progress.journey, kJourneySteps);
        hc.note = step >= kJourneySteps ? std::string{"JOURNEY COMPLETE - THE ROAD GOES ON"}
                                        : std::format("JOURNEY: {}", journey_step(step).title);
        hc.color = player_color(hh.color);
        hc.accent = role_color(hh.role);
        hc.xp_frac = level_progress(hh.progress.xp);
        hc.selected = static_cast<int>(i) == roster_.selected;
        const PlayerRole hr = hh.role;
        hc.icon = [this, hr](ui::DrawList& dl, Vec2 c, f32 r) {
            draw_ability_icon(dl, hr, 0, c.x, c.y, r, Vec4{glm::mix(role_color(hr), Vec3{1.0f}, 0.15f), 1.0f});
        };
        hc.on_click = [this, i] {
            select_hero(static_cast<int>(i));
            confirm_delete_ = false;
            rebuild_ui();
        };
        y += ch + cgap;
    }
    if (can_add) {
        auto& nc = panel.add<HeroCard>();
        nc.bounds = ui::Rect{x, y, cwid, std::min(ch, 64.0f)};
        nc.is_new = true;
        nc.name = "FORGE A NEW HERO";
        nc.sub = any ? "A FRESH START - LEVEL 1, TWO STARTING SKILLS" : "EVERY LEGEND STARTS SOMEWHERE";
        nc.on_click = [this] { begin_new_hero(); };
    }

    auto& join = panel.add<ui::Button>("JOIN", [this] { show_screen(Screen::Join); });
    join.bounds = ui::Rect{x, row1, half, bh};
    join.enabled = any;
    auto& host = panel.add<ui::Button>("HOST", [this] { enter_game(true, "127.0.0.1"); });
    host.primary = true;
    host.bounds = ui::Rect{x + half + bgap, row1, half, bh};
    host.enabled = any;
    auto& edit = panel.add<ui::Button>("EDIT LOOK", [this] {
        creating_ = false;
        show_screen(Screen::Customise);
    });
    edit.bounds = ui::Rect{x, row2, half, bh};
    edit.enabled = any;
    auto& del = panel.add<ui::Button>(confirm_delete_ ? "REALLY DELETE?" : "DELETE", [this] {
        if (!confirm_delete_) {
            confirm_delete_ = true; // ask once more - a hero's whole journey is at stake
            rebuild_ui();
            return;
        }
        confirm_delete_ = false;
        if (roster_.selected >= 0 && roster_.selected < static_cast<int>(roster_.heroes.size())) {
            roster_.heroes.erase(roster_.heroes.begin() + roster_.selected);
            roster_.selected = std::max(0, roster_.selected - 1);
            save_roster(roster_);
        }
        hero_index_ = -1;
        if (!roster_.heroes.empty()) {
            select_hero(roster_.selected);
        }
        rebuild_ui();
    });
    del.bounds = ui::Rect{x + half + bgap, row2, half, bh};
    del.enabled = any;
    panel.add<ui::Button>("BACK", [this] {
             confirm_delete_ = false;
             show_screen(Screen::Main);
         }).bounds = ui::Rect{x, row3, cwid, bh};

    // A nameplate under the turntable: the chosen hero's name in their colour.
    if (any) {
        const Hero& hh = roster_.heroes[static_cast<usize>(roster_.selected)];
        auto& nm = ui_.root().add<ui::Label>(hh.name, 30.0f, ui::TextAlign::Center);
        nm.bounds = ui::Rect{0.0f, h - 96.0f, card.x, 34.0f};
        nm.face = ui::FontFace::Display;
        nm.color = Vec4{glm::mix(player_color(hh.color), Vec3{1.0f}, 0.35f), 1.0f};
        auto& sub = ui_.root().add<ui::Label>(
            std::format("LEVEL {} {}  -  {} RAIDERS FELLED  -  {} WAGONS DELIVERED", hh.level(), role_name(hh.role),
                        hh.progress.kills, hh.progress.deliveries),
            12.0f, ui::TextAlign::Center);
        sub.bounds = ui::Rect{0.0f, h - 56.0f, card.x, 16.0f};
        sub.color = ui::theme().text_muted;
        sub.face = ui::FontFace::Bold;
        sub.tracking = 0.12f;
    } else {
        auto& hint = ui_.root().add<ui::Label>("FORGE YOUR FIRST HERO TO BEGIN", 20.0f, ui::TextAlign::Center);
        hint.bounds = ui::Rect{0.0f, h * 0.5f, card.x, 24.0f};
        hint.color = ui::theme().text_muted;
    }
}

void ClientApp::select_hero(int index) {
    if (roster_.heroes.empty()) {
        hero_index_ = -1;
        return;
    }
    index = std::clamp(index, 0, static_cast<int>(roster_.heroes.size()) - 1);
    roster_.selected = index;
    hero_index_ = index;
    hero_ = roster_.heroes[static_cast<usize>(index)];
    tidy_bar(hero_);
    role_ = hero_.role;
    appearance_ = hero_.appearance;
    equip_loadout_.outfit_tint = hero_.outfit_tint;
    equip_loadout_.weapon_index = hero_.weapon_index;
    for (usize i = 0; i < kAbilitySlots; ++i) {
        bar_[i] = hero_.bar[i];
    }
    rebuild_preview();
}

void ClientApp::begin_new_hero() {
    creating_ = true;
    confirm_delete_ = false;
    hero_index_ = -1;
    role_ = PlayerRole::Knight;
    hero_ = make_hero(role_);
    hero_.name.clear(); // typed on the next screen (a default is offered)
    // A colour none of the other heroes wear, so a couch co-op roster reads distinct.
    for (u8 c = 0; c < kPlayerColorCount; ++c) {
        bool used = false;
        for (const Hero& o : roster_.heroes) {
            used = used || o.color == c;
        }
        if (!used) {
            hero_.color = c;
            break;
        }
    }
    appearance_ = CharacterAppearance{};
    equip_loadout_.outfit_tint = hero_.outfit_tint;
    equip_loadout_.weapon_index = 0;
    show_screen(Screen::Class);
}

void ClientApp::commit_hero() {
    const bool fresh = hero_index_ < 0;
    if (fresh) {
        // A new hero starts its kit + bar for the class chosen in step 1.
        const std::string name = hero_.name;
        const u8 color = hero_.color;
        hero_ = make_hero(role_);
        hero_.name = name;
        hero_.color = color;
    }
    // Trim the name; fall back to a fitting default.
    while (!hero_.name.empty() && hero_.name.back() == ' ') {
        hero_.name.pop_back();
    }
    if (hero_.name.empty()) {
        hero_.name = default_hero_name(role_, roster_.heroes.size());
    }
    hero_.role = role_;
    hero_.appearance = appearance_;
    hero_.outfit_tint = equip_loadout_.outfit_tint;
    hero_.weapon_index = equip_loadout_.weapon_index;
    tidy_bar(hero_);
    if (fresh) {
        if (roster_.heroes.size() >= kMaxHeroes) {
            roster_.heroes.pop_back(); // full roster: the newest replaces the last slot
        }
        roster_.heroes.push_back(hero_);
        hero_index_ = static_cast<int>(roster_.heroes.size()) - 1;
    } else if (hero_index_ < static_cast<int>(roster_.heroes.size())) {
        roster_.heroes[static_cast<usize>(hero_index_)] = hero_;
    }
    roster_.selected = hero_index_;
    save_roster(roster_);
    select_hero(hero_index_);
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
    // While choosing a look the helm / hood / hat comes off, so the hair, eyes and ears being picked
    // actually show (the master helms enclose the whole head).
    eq.bare_head = current_screen_ == Screen::Customise;
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
    // Full-length on the roster; in the creator the camera closes in on the head + shoulders so the
    // eyes / ears / hair being chosen are big enough to see (eased between the two).
    const f32 z = glm::smoothstep(0.0f, 1.0f, preview_zoom_);
    const f32 ty = height * glm::mix(0.52f, 0.8f, z);      // look at the middle .. the face
    const f32 half_h = height * glm::mix(0.78f, 0.42f, z); // half-height + headroom (helms, a raised weapon)
    const f32 half_w = glm::mix(0.6f, 0.44f, z);           // generous half-width (shield / bow)
    const f32 free_frac = glm::clamp(panel_left / W, 0.25f, 1.0f);

    // Distance that fits both the height and the (panel-limited) width, + margin.
    const f32 dist_v = half_h / tan_v;
    const f32 dist_h = half_w / (tan_v * aspect * std::max(free_frac * 0.85f, 0.1f));
    const f32 dist = std::max(dist_v, dist_h) * 1.12f;

    // Offset the look-at so world x=0 projects to the centre of the free area.
    const f32 ndc_x = 2.0f * (panel_left * 0.5f / W) - 1.0f;
    const f32 target_x = -ndc_x * tan_v * aspect * dist;

    const Vec3 target{target_x, ty, 0.0f};
    const Vec3 eye = target + Vec3{0.0f, glm::mix(ty * 0.35f, 0.12f, z), dist};
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
    // A cool rim light from behind + above, so the silhouette separates from the dark backdrop and the
    // forms read (the warm key is the sun - see set_preview_studio).
    Renderer::SpotLight rim;
    rim.position = Vec3{-1.2f, height * 1.6f, -2.6f};
    rim.direction = glm::normalize(Vec3{0.0f, height * 0.6f, 0.0f} - rim.position);
    rim.color = Vec3{0.55f, 0.68f, 1.0f} * 1.6f;
    rim.range = 9.0f;
    rim.cone_outer_cos = std::cos(glm::radians(55.0f));
    rim.cone_inner_cos = std::cos(glm::radians(30.0f));
    rim.cast_shadow = false;
    renderer_->add_light(rim);

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

void ClientApp::set_preview_studio() {
    // The haze is measured from the "player" (see mesh.frag fogFactor) - and the menu backdrop parks
    // that reference on the showcase town it orbits, hundreds of metres from the turntable at the
    // origin. Left as it was, the hero stood deep inside the fog and washed out to a pale cream
    // silhouette (only when you came through the main menu, which is why a straight-to-the-creator
    // run looked fine). So the turntable gets a clean studio: no haze, dry ground, no cloud shadow,
    // and a warm, slightly softened key light from the front.
    renderer_->set_player_position(Vec3{0.0f});
    renderer_->set_fog(menu_sky_, 0.0f, 0.0f, 0.0f);
    renderer_->set_cloud_cover(0.0f);
    renderer_->set_wetness(0.0f);
    renderer_->set_wind(0.12f);
    renderer_->set_sun(glm::normalize(Vec3{0.35f, 0.85f, 0.45f}), Vec3{1.0f, 0.94f, 0.86f}, 0.88f);
}

// ---- The living backdrop behind the menus ---------------------------------------------------

void ClientApp::update_menu_scene(Timestep dt) {
    if (renderer_ == nullptr) {
        return;
    }
    if (!menu_terrain_) {
        // The showcase town: the first walled market town found spiralling out from the world origin
        // (not a hamlet's few cottages).
        std::optional<worldgen::Village> town;
        for (int r = 0; r <= 8 && !town; ++r) {
            for (int vz = -r; vz <= r && !town; ++vz) {
                for (int vx = -r; vx <= r && !town; ++vx) {
                    if (std::max(std::abs(vx), std::abs(vz)) == r) {
                        const auto v = worldgen::village_at(vx, vz, kMenuSeed);
                        if (v && v->tier == worldgen::TownTier::Town) {
                            town = v;
                        }
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
