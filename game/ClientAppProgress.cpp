// ClientApp - the hero's progression on the client: the skill tree (K), the saved hero kept in step
// with the server, level-up / journey celebrations, and the identity-colour readouts that tell
// players apart (rings at their feet, name plates, off-screen pointers, party frames).
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"
#include "HudStyle.h"

#include <bit>
#include <cctype>
#include <functional>

namespace alryn::game {

namespace {
const char* kTierNames[kSkillTiers] = {"TIER I", "TIER II", "TIER III", "TIER IV"};

// A small padlock glyph centred at `c` (locked skill nodes / elements).
void draw_lock(ui::DrawList& d, const Vec2& c, f32 s, const Vec4& col) {
    const Vec4 body{c.x - s * 0.55f, c.y - s * 0.1f, s * 1.1f, s * 0.85f};
    d.line(Vec2{c.x - s * 0.32f, c.y - s * 0.1f}, Vec2{c.x - s * 0.32f, c.y - s * 0.42f}, s * 0.18f, col);
    d.line(Vec2{c.x + s * 0.32f, c.y - s * 0.1f}, Vec2{c.x + s * 0.32f, c.y - s * 0.42f}, s * 0.18f, col);
    d.line(Vec2{c.x - s * 0.32f, c.y - s * 0.42f}, Vec2{c.x + s * 0.32f, c.y - s * 0.42f}, s * 0.18f, col);
    d.rect(body, col, s * 0.15f);
    d.rect(Vec4{c.x - s * 0.07f, c.y + s * 0.1f, s * 0.14f, s * 0.3f}, Vec4{0.05f, 0.03f, 0.02f, 0.9f}, s * 0.05f);
}
} // namespace

// ---- The hero, kept in step with the server ---------------------------------------------------

u8 ClientApp::known_mask() const {
    const u8 learned = restore_acked_ ? live_progress_.known : hero_.progress.known;
    return static_cast<u8>(learned | starter_mask(role_));
}

u8 ClientApp::hero_level() const { return restore_acked_ ? live_level_ : hero_.level(); }

void ClientApp::sync_hero_progress(bool force_save) {
    if (!restore_acked_) {
        return; // until the server has adopted our save, what it reports isn't ours to keep
    }
    std::array<int, kAbilitySlots> bar{};
    for (usize i = 0; i < kAbilitySlots; ++i) {
        bar[i] = bar_[i];
    }
    const bool changed = !(hero_.progress == live_progress_) || bar != hero_.bar || session_time_ > 30.0f;
    hero_.progress = live_progress_;
    hero_.bar = bar;
    hero_.played_seconds += static_cast<u32>(session_time_);
    session_time_ = 0.0f;
    if (hero_index_ < 0 || hero_index_ >= static_cast<int>(roster_.heroes.size())) {
        return; // a scripted run's stand-in hero isn't in the roster - nothing to save
    }
    roster_.heroes[static_cast<usize>(hero_index_)] = hero_;
    if (changed || force_save) {
        save_roster(roster_);
    }
}

void ClientApp::update_progress_fx(Timestep dt) {
    session_time_ += dt.seconds;
    levelup_fx_ = std::max(0.0f, levelup_fx_ - dt.seconds);
    journey_fx_ = std::max(0.0f, journey_fx_ - dt.seconds);
    if (!restore_acked_) {
        return;
    }
    if (last_level_seen_ == 0) {
        // First trusted snapshot: adopt the restored values without celebrating them.
        last_level_seen_ = live_level_;
        last_journey_seen_ = live_progress_.journey;
    } else {
        const Vec3 feet = local_feet();
        if (live_level_ > last_level_seen_) {
            levelup_fx_ = 4.5f;
            if (Audio* a = audio()) {
                a->play(SfxId::LevelUp, 0.95f);
            }
            // A pillar of gold light, a rune circle and a fountain of sparks.
            const Vec4 gold{1.0f, 0.82f, 0.36f, 1.0f};
            beam(feet + Vec3{0.0f, 14.0f, 0.0f}, feet, Vec4{1.0f, 0.86f, 0.45f, 0.9f}, 0.55f, 1.1f, 2);
            glyph(feet, 2.2f, gold, 1.8f, 8);
            emit_ring(feet + Vec3{0.0f, 0.1f, 0.0f}, gold, 48, 5.5f, 0.8f, 0.18f);
            emit_burst(feet + Vec3{0.0f, 1.0f, 0.0f}, gold, 60, 4.0f, 1.1f, 0.14f, 1, 5.0f, 4.0f);
            flash_light(feet + Vec3{0.0f, 2.0f, 0.0f}, Vec3{1.0f, 0.8f, 0.4f}, 6.0f, 14.0f, 1.2f);
            combat_text(feet, std::format("LEVEL {}!", live_level_), gold, 30.0f);
        }
        if (live_progress_.journey > last_journey_seen_ && last_journey_seen_ < kJourneySteps) {
            journey_fx_ = 4.0f;
            journey_fx_step_ = last_journey_seen_;
            if (levelup_fx_ < 4.0f) { // the level-up jingle already covers a simultaneous step
                if (Audio* a = audio()) {
                    a->play(SfxId::LevelUp, 0.6f, 0.8f);
                }
            }
        }
        last_level_seen_ = live_level_;
        last_journey_seen_ = live_progress_.journey;
    }
    hero_save_cd_ -= dt.seconds;
    if (hero_save_cd_ <= 0.0f) {
        hero_save_cd_ = 10.0f;
        sync_hero_progress(false);
    }
}

void ClientApp::draw_progress_fx(ui::DrawList& draw, f32 W, f32 H) {
    const f32 bs = glm::clamp(H * 0.05f, 24.0f, 56.0f);
    if (levelup_fx_ > 0.0f) {
        const f32 y = H * 0.15f;
        hud::banner(draw, W * 0.5f, y, std::format("LEVEL {}", live_level_), bs, hud::kGold);
        const i32 pts = points_available(role_, live_level_, known_mask(), live_progress_.talents);
        std::string sub = pts > 0 ? std::format("{} SKILL POINT{} TO SPEND - OPEN THE SKILL TREE [K]", pts,
                                                pts == 1 ? "" : "S")
                                  : std::string{"STRONGER, TOUGHER - THE ROADS GROW MORE DANGEROUS"};
        for (u8 t = 1; t < kSkillTiers; ++t) {
            if (kTierLevel[t] == live_level_) {
                sub = std::format("{} SKILLS UNLOCKED - OPEN THE SKILL TREE [K]", kTierNames[t]);
            }
        }
        const f32 ss = bs * 0.42f;
        hud::rich(draw, Vec2{(W - hud::rich_width(sub, ss)) * 0.5f, y + bs * 2.1f}, sub, ss, ui::theme().text);
    }
    if (journey_fx_ > 0.0f) {
        // A toast that slides out from under the objective card (where the journey lives): the goal
        // just met, its reward and what's next - out of the way of the world + any contract panel.
        const JourneyStep done = journey_step(journey_fx_step_);
        const f32 t = 4.0f - journey_fx_;
        const f32 slide = glm::clamp(t / 0.35f, 0.0f, 1.0f) * glm::clamp(journey_fx_ / 0.5f, 0.0f, 1.0f);
        const f32 ts = glm::clamp(H * 0.026f, 15.0f, 30.0f);
        const std::string head = std::format("GOAL MET: {}", done.title);
        const std::string next = std::format("+{} XP   -   NEXT: {}", done.xp,
                                             journey_step(static_cast<u8>(journey_fx_step_ + 1)).title);
        const f32 w = std::max(hud::heading_width(head, ts * 0.8f), hud::width(next, ts * 0.55f)) + ts * 2.0f;
        const f32 h = ts * 2.6f;
        const Vec4 r{16.0f - (1.0f - slide) * (w + 30.0f), objective_bottom_ + 10.0f, w, h};
        draw.shadow(r, 9.0f, 14.0f, hud::alpha(hud::kGood, 0.35f * slide));
        hud::plaque(draw, r, 0.9f);
        draw.rect(Vec4{r.x + 3.0f, r.y + 5.0f, 4.0f, h - 10.0f}, hud::kGood, 2.0f);
        hud::heading(draw, Vec2{r.x + ts, r.y + ts * 0.32f}, head, ts * 0.8f);
        hud::text(draw, Vec2{r.x + ts, r.y + ts * 1.45f}, next, ts * 0.55f, hud::kGold);
    }
}

// ---- Identity colours in the world -----------------------------------------------------------

void ClientApp::draw_player_rings() {
    if (!have_snapshot_ || renderer_ == nullptr) {
        return;
    }
    const net::WagonState* aw = active_wagon();
    for (const net::PlayerState& p : snapshot_.players) {
        if (p.seated != 0 && aw != nullptr) {
            continue; // riding the cart - the ring would hang in the air off the bench
        }
        const Vec3 col = player_color(p.color);
        const bool me = p.id == my_id_;
        // Lie the ring on the slope under them (when they stand on the terrain, not a deck/bridge),
        // so it never half-buries itself on a hillside.
        Mat4 tilt{1.0f};
        if (world_seed_ != 0) {
            const f32 x = p.position.x, z = p.position.z;
            const f32 h0 = worldgen::height(x, z, world_seed_);
            if (std::abs(h0 - p.position.y) < 0.35f) {
                const f32 dx = worldgen::height(x + 0.5f, z, world_seed_) - worldgen::height(x - 0.5f, z, world_seed_);
                const f32 dz = worldgen::height(x, z + 0.5f, world_seed_) - worldgen::height(x, z - 0.5f, world_seed_);
                const Vec3 n = glm::normalize(Vec3{-dx, 1.0f, -dz});
                const Vec3 axis = glm::cross(Vec3{0.0f, 1.0f, 0.0f}, n);
                const f32 s = glm::length(axis);
                if (s > 1e-4f) {
                    tilt = glm::rotate(Mat4{1.0f}, std::asin(std::min(s, 1.0f)), axis / s);
                }
            }
        }
        const Mat4 at = glm::translate(Mat4{1.0f}, p.position + Vec3{0.0f, 0.05f, 0.0f}) * tilt;
        // A crisp self-lit band in the player's own colour (true to the swatch day or night), with a
        // faint glow pool inside it. Teammates' rings are bolder; your own is a quieter guide.
        const f32 k = me ? 0.75f : 1.0f;
        renderer_->draw_emissive(ring_mesh_, at * glm::scale(Mat4{1.0f}, Vec3{me ? 0.9f : 1.0f}), Vec4{col * 0.9f * k, 1.0f});
        renderer_->draw_glow(shape_cylinder_, at * glm::scale(Mat4{1.0f}, Vec3{0.62f, 0.02f, 0.62f}),
                             Vec4{col, (me ? 0.10f : 0.16f)});
    }
}

void ClientApp::draw_nameplates(ui::DrawList& draw, f32 W, f32 H) {
    if (!have_snapshot_) {
        return;
    }
    const Mat4 vp = camera_.view_projection();
    const Vec3 me = local_feet();
    const f32 inset = 46.0f;
    for (const net::PlayerState& p : snapshot_.players) {
        if (p.id == my_id_) {
            continue;
        }
        const Vec3 col = player_color(p.color);
        const Vec4 text_col{glm::mix(col, Vec3{1.0f}, 0.3f), 1.0f};
        const f32 hgt = p.appearance.race == Race::Dwarf ? 1.95f : 2.35f;
        const Vec4 clip = vp * Vec4{p.position + Vec3{0.0f, hgt, 0.0f}, 1.0f};
        const bool front = clip.w > 0.05f;
        const Vec2 ndc = front ? Vec2{clip.x / clip.w, clip.y / clip.w} : Vec2{0.0f};
        const std::string name = p.name.empty() ? std::format("PLAYER {}", p.id) : p.name;
        if (front && std::abs(ndc.x) < 0.98f && std::abs(ndc.y) < 0.98f) {
            // On screen: name + level over their head, a health sliver underneath.
            const Vec2 sp{(ndc.x * 0.5f + 0.5f) * W, (ndc.y * 0.5f + 0.5f) * H};
            const f32 ns = 14.0f;
            const std::string lv = std::format("{}", p.level);
            const f32 nw = hud::width(name, ns);
            const f32 lw = ns * 1.45f;
            const f32 total = nw + lw + 6.0f;
            const f32 x0 = sp.x - total * 0.5f;
            // Level badge: a disc in the player's colour.
            const Vec4 badge{x0, sp.y - ns * 0.25f, lw, lw};
            draw.gradient(badge, Vec4{glm::mix(col, Vec3{1.0f}, 0.2f), 1.0f}, Vec4{col * 0.6f, 1.0f}, lw * 0.5f,
                          Vec4{0.05f, 0.03f, 0.02f, 0.9f}, 1.25f);
            hud::text(draw, Vec2{badge.x + lw * 0.5f, badge.y + (lw - ns * 0.72f) * 0.5f}, lv, ns * 0.72f,
                      Vec4{1.0f}, ui::TextAlign::Center);
            hud::text(draw, Vec2{x0 + lw + 6.0f, sp.y}, name, ns, text_col);
            const f32 hp = static_cast<f32>(p.health) / 100.0f;
            const f32 bw = std::max(nw, 48.0f);
            const Vec4 bar{x0 + lw + 6.0f, sp.y + ns + 5.0f, bw, 4.0f};
            draw.rect(Vec4{bar.x - 1.0f, bar.y - 1.0f, bar.z + 2.0f, bar.w + 2.0f}, Vec4{0.03f, 0.02f, 0.01f, 0.85f}, 2.5f);
            const Vec3 hc = glm::mix(Vec3{0.9f, 0.25f, 0.2f}, Vec3{0.42f, 0.86f, 0.36f}, hp);
            if (hp > 0.0f) {
                draw.rect(Vec4{bar.x, bar.y, bar.z * hp, bar.w}, Vec4{hc, 1.0f}, 2.0f);
            }
            continue;
        }
        // Off screen: a pointer at the edge, toward them, with their initial + distance.
        Vec2 dir = front ? ndc : Vec2{-clip.x, -clip.y};
        if (glm::length(dir) < 1e-4f) {
            dir = Vec2{0.0f, 1.0f};
        }
        dir = glm::normalize(dir);
        const f32 hx = W * 0.5f - inset;
        const f32 hy = H * 0.5f - inset;
        const f32 t = std::min(std::abs(dir.x) > 1e-4f ? hx / std::abs(dir.x) : 1e9f,
                               std::abs(dir.y) > 1e-4f ? hy / std::abs(dir.y) : 1e9f);
        const Vec2 c = Vec2{W * 0.5f, H * 0.5f} + dir * t;
        const f32 r = 15.0f;
        const Vec2 perp{-dir.y, dir.x};
        const Vec2 tip = c + dir * (r + 11.0f);
        const Vec4 cc{col, 0.95f};
        draw.line(c + dir * r * 0.6f + perp * r * 0.62f, tip, 4.0f, cc);
        draw.line(c + dir * r * 0.6f - perp * r * 0.62f, tip, 4.0f, cc);
        draw.rect(Vec4{c.x - r - 2.0f, c.y - r - 2.0f, (r + 2.0f) * 2.0f, (r + 2.0f) * 2.0f},
                  Vec4{0.05f, 0.03f, 0.02f, 0.85f}, r + 2.0f);
        draw.gradient(Vec4{c.x - r, c.y - r, r * 2.0f, r * 2.0f}, Vec4{glm::mix(col, Vec3{1.0f}, 0.25f), 1.0f},
                      Vec4{col * 0.65f, 1.0f}, r);
        const std::string initial = name.substr(0, 1);
        hud::text(draw, Vec2{c.x, c.y - 8.0f}, initial, 16.0f, Vec4{1.0f}, ui::TextAlign::Center,
                  ui::FontFace::Display);
        const int dist = static_cast<int>(glm::length(p.position - me));
        hud::text(draw, Vec2{c.x - dir.x * (r + 14.0f), c.y - dir.y * (r + 14.0f) - 6.0f}, std::format("{}M", dist),
                  11.0f, text_col, ui::TextAlign::Center);
    }
}

void ClientApp::draw_party_frames(ui::DrawList& draw, f32 W, f32 H, f32 ts) {
    if (!have_snapshot_ || snapshot_.players.size() < 2) {
        return;
    }
    (void)W;
    const f32 fw = std::max(ts * 9.5f, 170.0f);
    const f32 fh = ts * 1.9f;
    f32 y = H * 0.38f;
    const f32 x = 16.0f;
    for (const net::PlayerState& p : snapshot_.players) {
        if (p.id == my_id_) {
            continue;
        }
        const Vec3 col = player_color(p.color);
        const Vec4 frame{x, y, fw, fh};
        hud::plaque(draw, frame, 0.78f, 7.0f);
        draw.rect(Vec4{frame.x + 3.0f, frame.y + 4.0f, 4.0f, fh - 8.0f}, Vec4{col, 1.0f}, 2.0f);
        const f32 cr = fh * 0.32f;
        const Vec2 cc{frame.x + 14.0f + cr, frame.y + fh * 0.5f};
        hud::medallion(draw, cc, cr, Vec4{0.06f, 0.04f, 0.03f, 1.0f});
        const auto role = static_cast<PlayerRole>(p.role % kRoleCount);
        draw_ability_icon(draw, role, 0, cc.x, cc.y, cr * 0.5f, Vec4{role_color(role), 1.0f});
        const f32 tx = cc.x + cr + 10.0f;
        const f32 ns = ts * 0.56f;
        const std::string name = p.name.empty() ? std::format("PLAYER {}", p.id) : p.name;
        hud::text(draw, Vec2{tx, frame.y + fh * 0.16f}, name, ns, Vec4{glm::mix(col, Vec3{1.0f}, 0.35f), 1.0f});
        hud::text(draw, Vec2{frame.x + fw - 8.0f, frame.y + fh * 0.18f}, std::format("LV {}", p.level), ns * 0.8f,
                  hud::kGold, ui::TextAlign::Right);
        const f32 hp = static_cast<f32>(p.health) / 100.0f;
        const Vec3 hc = glm::mix(Vec3{0.88f, 0.22f, 0.18f}, Vec3{0.38f, 0.8f, 0.32f}, hp);
        hud::bar(draw, Vec4{tx, frame.y + fh * 0.62f, frame.x + fw - tx - 9.0f, fh * 0.2f}, hp,
                 Vec4{glm::mix(hc, Vec3{1.0f}, 0.25f), 1.0f}, Vec4{hc * 0.68f, 1.0f});
        y += fh + 6.0f;
    }
}

// ---- The skill tree (K) ------------------------------------------------------------------------

void ClientApp::draw_skills() {
    if (renderer_ == nullptr) {
        return;
    }
    const VkExtent2D ext = renderer_->extent();
    const f32 W = static_cast<f32>(ext.width);
    const f32 H = static_cast<f32>(ext.height);
    ui::DrawList draw{*renderer_};
    const ui::Theme& th = ui::theme();
    const Vec3 accent = role_color(role_);
    const Vec2 mouse = pointer_pos();

    const u8 level = hero_level();
    const u8 known = known_mask();
    const u8 talents = restore_acked_ ? live_progress_.talents : hero_.progress.talents;
    const u32 xp = restore_acked_ ? live_progress_.xp : hero_.progress.xp;
    const i32 points = points_available(role_, level, known, talents);
    const Vec3 sk_feet = local_feet();
    const bool in_town = world_seed_ != 0 && worldgen::inside_village(sk_feet.x, sk_feet.z, world_seed_, 6.0f);

    const f32 mg = std::min(W, H) * 0.045f;
    const Vec4 panel{mg, mg, W - 2.0f * mg, H - 2.0f * mg};
    f32 hy = hud::window(draw, W, H, panel, "SKILL TREE");
    hud::text(draw, Vec2{panel.x + panel.z - 34.0f, panel.y + 34.0f},
              std::format("{}  -  {}", role_name(role_), role_desc(role_)), 14.0f,
              Vec4{glm::mix(accent, Vec3{1.0f}, 0.3f), 1.0f}, ui::TextAlign::Right);

    // ---- Header: the hero (crest, name, level), their XP and the points to spend.
    const f32 hx = panel.x + 34.0f;
    {
        const f32 cr = 24.0f;
        const Vec2 cc{hx + cr, hy + cr + 2.0f};
        hud::medallion(draw, cc, cr, Vec4{0.07f, 0.05f, 0.035f, 1.0f});
        draw.glow(Vec4{cc.x - cr * 1.6f, cc.y - cr * 1.6f, cr * 3.2f, cr * 3.2f}, Vec4{accent, 0.25f}, Vec4{accent, 0.0f});
        draw_ability_icon(draw, role_, 0, cc.x, cc.y, cr * 0.5f, Vec4{accent, 1.0f});
        const Vec3 pc = player_color(restore_acked_ ? live_color_ : hero_.color);
        const f32 nx = cc.x + cr + 16.0f;
        hud::text(draw, Vec2{nx, hy + 2.0f}, hero_.name, 22.0f, Vec4{glm::mix(pc, Vec3{1.0f}, 0.35f), 1.0f},
                  ui::TextAlign::Left, ui::FontFace::Display);
        hud::text(draw, Vec2{nx, hy + 30.0f}, std::format("LEVEL {}  {}", level, role_name(role_)), 13.0f, th.text);
        // XP gauge.
        const f32 bx = nx + 230.0f;
        const f32 bw = std::min(340.0f, panel.x + panel.z - bx - 260.0f);
        if (bw > 80.0f) {
            const f32 frac = level_progress(xp);
            hud::bar(draw, Vec4{bx, hy + 14.0f, bw, 14.0f}, frac, Vec4{1.0f, 0.9f, 0.55f, 1.0f},
                     Vec4{0.78f, 0.55f, 0.2f, 1.0f}, 10);
            const std::string xs = level >= kMaxLevel ? std::string{"MAX LEVEL"}
                                                      : std::format("XP  {} / {}", xp - xp_to_reach(level), xp_step(level));
            hud::text(draw, Vec2{bx + bw * 0.5f, hy + 33.0f}, xs, 11.0f, th.text_muted, ui::TextAlign::Center);
        }
        // Skill points chip (pulses while there's something to spend).
        const std::string ps = std::format("SKILL POINTS  {}", std::max(points, 0));
        const f32 cs = 15.0f;
        const f32 cw = hud::width(ps, cs) + cs * 1.2f;
        const Vec4 pcol = points > 0 ? Vec4{1.0f, 0.85f + 0.1f * std::sin(elapsed_ * 5.0f), 0.45f, 1.0f} : th.text_muted;
        if (points > 0) {
            draw.shadow(Vec4{panel.x + panel.z - 34.0f - cw, hy + 6.0f, cw, cs * 1.7f}, cs, 10.0f,
                        hud::alpha(hud::kGold, 0.35f + 0.2f * std::sin(elapsed_ * 5.0f)));
        }
        hud::chip(draw, Vec2{panel.x + panel.z - 34.0f - cw, hy + 6.0f}, ps, cs, pcol);
    }
    hy += 64.0f;
    draw.line(Vec2{panel.x + 30.0f, hy}, Vec2{panel.x + panel.z - 30.0f, hy}, 1.0f, hud::alpha(th.accent, 0.35f));
    hy += 12.0f;

    // ---- The tree (left) and the detail + talents column (right).
    const f32 body_bot = panel.y + panel.w - 44.0f;
    const f32 split = panel.x + panel.z * 0.6f;
    const f32 tree_x0 = panel.x + 150.0f;
    const f32 tree_w = split - 20.0f - tree_x0;
    const f32 row_h = (body_bot - hy) / static_cast<f32>(kSkillTiers);
    const f32 nb = glm::clamp(row_h * 0.46f, 38.0f, 66.0f); // node icon box
    auto node_center = [&](u8 a) {
        const SkillNode n = skill_node(role_, a);
        const f32 cx = n.tier == kSkillTiers - 1 ? tree_x0 + tree_w * 0.5f
                                                 : tree_x0 + tree_w * (n.column == 0 ? 0.26f : 0.74f);
        return Vec2{cx, hy + row_h * static_cast<f32>(n.tier) + row_h * 0.36f};
    };

    // Tier labels, with their level gate.
    for (u8 t = 0; t < kSkillTiers; ++t) {
        const f32 ty = hy + row_h * static_cast<f32>(t) + row_h * 0.36f - 14.0f;
        const bool open = level >= kTierLevel[t];
        hud::text(draw, Vec2{panel.x + 40.0f, ty}, kTierNames[t], 15.0f, open ? th.title : th.text_muted,
                  ui::TextAlign::Left, ui::FontFace::Display);
        const std::string gate = t == 0 ? std::string{"STARTING SKILLS"} : std::format("LEVEL {}", kTierLevel[t]);
        hud::text(draw, Vec2{panel.x + 40.0f, ty + 21.0f}, gate, 10.5f,
                  open ? hud::kGood : hud::alpha(th.text_muted, 0.8f));
        if (t > 0) {
            const f32 ly = hy + row_h * static_cast<f32>(t) - 2.0f;
            draw.line(Vec2{panel.x + 36.0f, ly}, Vec2{split - 24.0f, ly}, 1.0f, hud::alpha(th.accent, 0.12f));
        }
    }

    // Links from each node to its parent (behind the nodes): lit when the child is learned.
    for (u8 a = 0; a < kAbilityCount; ++a) {
        const SkillNode n = skill_node(role_, a);
        if (n.prereq < 0) {
            continue;
        }
        const Vec2 pc = node_center(static_cast<u8>(n.prereq));
        const Vec2 cc = node_center(a);
        const Vec2 from{pc.x, pc.y + nb * 0.5f};
        const Vec2 to{cc.x, cc.y - nb * 0.5f};
        const f32 my = (from.y + to.y) * 0.5f;
        const bool lit = knows(known, a);
        const bool next = !lit && knows(known, static_cast<u8>(n.prereq));
        const Vec4 col = lit ? hud::alpha(th.accent_hover, 0.9f) : (next ? hud::alpha(th.accent, 0.55f) : hud::alpha(th.accent, 0.2f));
        for (const auto& [wdt, c] : {std::pair{5.0f, Vec4{0.02f, 0.015f, 0.01f, 0.6f}}, std::pair{2.25f, col}}) {
            draw.line(from, Vec2{from.x, my}, wdt, c);
            draw.line(Vec2{from.x, my}, Vec2{to.x, my}, wdt, c);
            draw.line(Vec2{to.x, my}, to, wdt, c);
        }
    }

    // The nodes: an icon box, the name, and its state (equipped key / LEARN / requirement).
    for (u8 a = 0; a < kAbilityCount; ++a) {
        const SkillNode n = skill_node(role_, a);
        const AbilityDef ab = ability_def(role_, a);
        const Vec2 c = node_center(a);
        const Vec4 ib{c.x - nb * 0.5f, c.y - nb * 0.5f, nb, nb};
        const bool is_known = knows(known, a);
        const bool learnable = can_learn(role_, a, level, known, talents);
        int bound = -1;
        for (u8 s = 0; s < kAbilitySlots; ++s) {
            if (bar_[s] == static_cast<int>(a)) {
                bound = static_cast<int>(s);
            }
        }
        const f32 name_sz = glm::clamp(nb * 0.22f, 11.0f, 14.0f);
        const f32 label_w = std::max(hud::width(ab.name, name_sz), nb);
        skill_node_rects_[a] = ui::Rect{c.x - label_w * 0.5f - 6.0f, ib.y - 4.0f, label_w + 12.0f, nb + name_sz * 3.4f + 8.0f};
        skill_learn_rects_[a] = learnable ? skill_node_rects_[a] : ui::Rect{};
        const bool hot = in_rect(mouse, skill_node_rects_[a]);
        if (hot) {
            skill_hover_ = a;
        }
        const bool focused = skill_hover_ == static_cast<int>(a);
        const f32 rad = nb * 0.18f;
        if (learnable) {
            const f32 pulse = 0.5f + 0.5f * std::sin(elapsed_ * 4.0f + static_cast<f32>(a));
            draw.shadow(ib, rad, 14.0f, hud::alpha(hud::kGold, 0.35f + 0.35f * pulse));
        } else if (is_known) {
            draw.shadow(ib, rad, 8.0f, hud::alpha(th.accent_hover, focused ? 0.45f : 0.25f));
        }
        draw.gradient(ib, is_known ? Vec4{0.15f, 0.11f, 0.075f, 0.98f} : Vec4{0.07f, 0.055f, 0.045f, 0.95f},
                      Vec4{0.05f, 0.035f, 0.025f, 0.98f}, rad,
                      is_known || learnable ? th.accent_hover : hud::alpha(th.accent, focused ? 0.55f : 0.3f),
                      focused ? 2.5f : (is_known ? 1.75f : 1.25f));
        if (is_known) {
            draw.glow(Vec4{ib.x + nb * 0.1f, ib.y + nb * 0.1f, nb * 0.8f, nb * 0.8f}, Vec4{accent, 0.3f}, Vec4{accent, 0.0f});
        }
        const Vec4 icol = is_known ? Vec4{glm::mix(accent, Vec3{1.0f}, 0.2f), 1.0f}
                                   : (learnable ? Vec4{accent * 0.85f, 0.95f} : Vec4{Vec3{0.42f, 0.38f, 0.34f}, 0.8f});
        draw_ability_icon(draw, role_, a, c.x, c.y, nb * 0.26f, icol);
        if (!is_known && !learnable) {
            draw_lock(draw, Vec2{ib.x + nb - 11.0f, ib.y + nb - 11.0f}, 9.0f, Vec4{0.72f, 0.64f, 0.52f, 0.95f});
        }
        if (bound >= 0 && role_ != PlayerRole::Mage) {
            hud::key_cap(draw, Vec2{ib.x - 7.0f, ib.y - 5.0f}, std::format("{}", bound + 1), nb * 0.17f);
        }
        // Name + state line under the box.
        const f32 ny = ib.y + nb + 6.0f;
        hud::text(draw, Vec2{c.x, ny}, ab.name, name_sz, is_known ? th.text : hud::alpha(th.text, learnable ? 0.9f : 0.55f),
                  ui::TextAlign::Center, ui::FontFace::Bold);
        const f32 ss = name_sz * 0.78f;
        std::string state;
        Vec4 scol = th.text_muted;
        if (is_known) {
            if (role_ == PlayerRole::Mage) {
                state = n.tier == 0 && a <= 3 ? "ELEMENT" : (a <= 3 ? "ELEMENT" : "COMBO");
            } else {
                state = bound >= 0 ? std::format("ON BAR [{}]", bound + 1) : "CLICK TO EQUIP";
            }
            scol = bound >= 0 ? hud::kGood : hud::alpha(th.accent_hover, 0.85f);
        } else if (learnable) {
            state = "CLICK TO LEARN";
            scol = hud::kGold;
        } else if (level < n.req_level) {
            state = std::format("LEVEL {}", n.req_level);
        } else if (n.prereq >= 0 && !knows(known, static_cast<u8>(n.prereq))) {
            state = std::format("NEEDS {}", ability_def(role_, static_cast<u8>(n.prereq)).name);
        } else {
            state = "NO POINTS";
        }
        if (role_ == PlayerRole::Mage && is_known) {
            scol = hud::alpha(th.accent_hover, 0.85f);
        }
        hud::text(draw, Vec2{c.x, ny + name_sz + 5.0f}, state, ss, scol, ui::TextAlign::Center);
    }

    // ---- Right column: the focused skill in full, then the talents.
    const f32 rx = split + 6.0f;
    const f32 rw = panel.x + panel.z - 34.0f - rx;
    const f32 det_h = (body_bot - hy) * 0.56f;
    const Vec4 det{rx, hy, rw, det_h - 10.0f};
    draw.gradient(det, Vec4{1.0f, 0.9f, 0.7f, 0.05f}, Vec4{1.0f, 0.9f, 0.7f, 0.015f}, 9.0f, hud::alpha(th.accent, 0.35f), 1.0f);
    if (skill_hover_ < 0 || skill_hover_ >= static_cast<int>(kAbilityCount)) {
        skill_hover_ = 0;
    }
    for (u8 a = 0; a < kAbilityCount; ++a) {
        skill_upgrade_rects_[a] = ui::Rect{};
    }
    {
        const u8 a = static_cast<u8>(skill_hover_);
        const AbilityDef ab = ability_def(role_, a);
        const SkillNode n = skill_node(role_, a);
        const bool is_known = knows(known, a);
        f32 y = det.y + 16.0f;
        const f32 x = det.x + 18.0f;
        const f32 iw = det.z - 36.0f;
        hud::text(draw, Vec2{x, y}, ab.name, 21.0f, is_known ? th.title : th.text, ui::TextAlign::Left, ui::FontFace::Display);
        y += 30.0f;
        f32 cx = x;
        cx += hud::chip(draw, Vec2{cx, y}, kTierNames[n.tier], 10.5f, hud::alpha(th.text_muted, 0.95f)) + 6.0f;
        cx += hud::chip(draw, Vec2{cx, y}, std::format("{:.0f}S COOLDOWN", ab.cooldown), 10.5f, hud::alpha(th.text_muted, 0.95f)) + 6.0f;
        hud::chip(draw, Vec2{cx, y}, is_known ? "LEARNED" : (can_learn(role_, a, level, known, talents) ? "1 POINT TO LEARN" : "LOCKED"),
                  10.5f, is_known ? hud::kGood : hud::kWarn);
        y += 28.0f;
        std::string desc{ab.desc};
        if (const char* mx = ability_rank_desc(role_, a); mx[0] != '\0' && ability_rank_[a] < kMaxAbilityRank) {
            desc += std::format("  ({})", mx);
        }
        for (const std::string& line : hud::wrap(desc, 13.0f, iw, 4)) {
            draw.text(Vec2{x, y}, line, 13.0f, th.text);
            y += 13.0f * 1.4f;
        }
        y += 6.0f;
        if (!is_known) {
            std::string req = std::format("REQUIRES  LEVEL {}", n.req_level);
            if (n.prereq >= 0) {
                req += std::format("  +  {}", ability_def(role_, static_cast<u8>(n.prereq)).name);
            }
            hud::text(draw, Vec2{x, y}, req, 11.5f, level >= n.req_level ? th.text_muted : hud::kWarn);
            y += 20.0f;
        }
        // Rank upgrades (bought with gold in a town) for the curated upgradeable skills.
        const u8 maxr = ability_max_rank(role_, a);
        if (maxr > 0) {
            const u8 rank = ability_rank_[a];
            const f32 rkw = hud::text(draw, Vec2{x, y + 1.0f}, "RANK", 11.5f, th.text_muted);
            f32 px = x + rkw + 12.0f;
            const f32 pip = 13.0f;
            for (u8 k = 0; k < maxr; ++k) {
                const Vec4 gr{px, y, pip, pip};
                if (k < rank) {
                    draw.shadow(gr, pip * 0.5f, 4.0f, hud::alpha(th.accent_hover, 0.5f));
                    draw.gradient(gr, th.accent_hover, hud::shade(th.accent, 0.7f), pip * 0.5f, hud::kInk, 1.0f);
                } else {
                    draw.gradient(gr, Vec4{0.03f, 0.02f, 0.015f, 0.95f}, Vec4{0.1f, 0.07f, 0.05f, 0.95f}, pip * 0.5f,
                                  hud::alpha(th.accent, 0.5f), 1.0f);
                }
                px += pip + 5.0f;
            }
            if (rank < maxr && is_known) {
                const u32 cost = ability_upgrade_price(static_cast<u8>(rank + 1));
                const bool can = in_town && snapshot_.money >= cost;
                const std::string label = std::format("UPGRADE  {}", cost);
                const f32 bsz = 12.0f;
                const f32 bw = hud::width(label, bsz) + bsz * 3.0f;
                const f32 bh = bsz + 14.0f;
                const Vec4 ub{px + 14.0f, y - (bh - pip) * 0.5f, bw, bh};
                if (can) {
                    draw.shadow(ub, bh * 0.3f, 6.0f, hud::alpha(th.accent_hover, 0.3f));
                }
                draw.gradient(ub, can ? th.accent_hover : Vec4{0.2f, 0.15f, 0.1f, 0.92f},
                              can ? hud::shade(th.accent, 0.72f) : Vec4{0.12f, 0.09f, 0.06f, 0.92f}, bh * 0.3f,
                              can ? hud::shade(th.accent, 0.5f) : hud::alpha(th.accent, 0.35f), 1.25f);
                hud::coin(draw, Vec2{ub.x + bsz * 1.0f, ub.y + bh * 0.5f}, bsz * 0.45f);
                ui::TextStyle us = hud::style(bsz, can ? th.accent_text : th.text_muted);
                us.outline = Vec4{0.0f};
                us.shadow = can ? Vec4{1.0f, 0.9f, 0.6f, 0.35f} : Vec4{0.0f, 0.0f, 0.0f, 0.5f};
                draw.text(Vec2{ub.x + bsz * 1.9f, ub.y + (bh - bsz) * 0.5f}, label, us);
                skill_upgrade_rects_[a] = ui::Rect{ub.x, ub.y, ub.z, ub.w};
                if (!in_town) {
                    hud::text(draw, Vec2{x, y + 22.0f}, "BUY RANKS IN A TOWN", 10.5f, hud::alpha(th.accent_hover, 0.85f));
                }
            } else if (rank >= maxr) {
                hud::text(draw, Vec2{px + 10.0f, y}, "MAX", 13.0f, hud::kGold);
            }
        }
        if (role_ == PlayerRole::Mage) {
            hud::rich(draw, Vec2{x, det.y + det.w - 30.0f}, "HOLD [CTRL] + ELEMENTS, RELEASE TO CAST", 11.0f, th.text_muted);
        }
    }

    // Talents: passive ranks bought with the same skill points.
    {
        f32 y = hy + det_h + 4.0f;
        const f32 x = rx + 4.0f;
        hud::heading(draw, Vec2{x, y}, "TALENTS", 17.0f);
        hud::text(draw, Vec2{x + hud::heading_width("TALENTS", 17.0f) + 14.0f, y + 5.0f},
                  "PASSIVE BONUSES - SPEND SKILL POINTS", 10.5f, th.text_muted);
        y += 30.0f;
        const f32 trow = std::min(54.0f, (body_bot - y) / static_cast<f32>(kTalentCount));
        for (u8 t = 0; t < kTalentCount; ++t) {
            const u8 rank = talent_rank(talents, t);
            const bool can = can_raise_talent(role_, t, level, known, talents);
            const Vec4 card{rx, y, rw, trow - 6.0f};
            draw.gradient(card, Vec4{1.0f, 0.9f, 0.7f, 0.04f}, Vec4{1.0f, 0.9f, 0.7f, 0.01f}, 7.0f,
                          hud::alpha(th.accent, can ? 0.5f : 0.2f), 1.0f);
            hud::text(draw, Vec2{card.x + 12.0f, card.y + 7.0f}, talent_name(t), 14.0f, rank > 0 ? th.title : th.text,
                      ui::TextAlign::Left, ui::FontFace::Display);
            hud::text(draw, Vec2{card.x + 12.0f, card.y + 27.0f}, talent_desc(t), 10.5f, th.text_muted);
            // Rank pips.
            const f32 pip = 11.0f;
            f32 px = card.x + card.z - 46.0f - static_cast<f32>(kMaxTalentRank) * (pip + 4.0f);
            for (u8 k = 0; k < kMaxTalentRank; ++k) {
                const Vec4 gr{px, card.y + (card.w - pip) * 0.5f, pip, pip};
                if (k < rank) {
                    draw.gradient(gr, th.accent_hover, hud::shade(th.accent, 0.7f), pip * 0.5f, hud::kInk, 1.0f);
                } else {
                    draw.gradient(gr, Vec4{0.03f, 0.02f, 0.015f, 0.95f}, Vec4{0.1f, 0.07f, 0.05f, 0.95f}, pip * 0.5f,
                                  hud::alpha(th.accent, 0.5f), 1.0f);
                }
                px += pip + 4.0f;
            }
            // "+" button (or what gates it).
            const Vec4 plus{card.x + card.z - 36.0f, card.y + (card.w - 26.0f) * 0.5f, 26.0f, 26.0f};
            talent_rects_[t] = ui::Rect{};
            if (rank < kMaxTalentRank) {
                if (can) {
                    const f32 pulse = 0.5f + 0.5f * std::sin(elapsed_ * 4.0f + static_cast<f32>(t));
                    draw.shadow(plus, 13.0f, 8.0f, hud::alpha(hud::kGold, 0.3f + 0.3f * pulse));
                    draw.gradient(plus, th.accent_hover, hud::shade(th.accent, 0.72f), 13.0f, hud::kInk, 1.0f);
                    draw.line(Vec2{plus.x + 7.0f, plus.y + 13.0f}, Vec2{plus.x + 19.0f, plus.y + 13.0f}, 3.0f, hud::kInk);
                    draw.line(Vec2{plus.x + 13.0f, plus.y + 7.0f}, Vec2{plus.x + 13.0f, plus.y + 19.0f}, 3.0f, hud::kInk);
                    talent_rects_[t] = ui::Rect{plus.x, plus.y, plus.z, plus.w};
                } else {
                    hud::text(draw, Vec2{plus.x + 13.0f, plus.y + 7.0f},
                              std::format("LV {}", talent_req_level(static_cast<u8>(rank + 1))), 10.0f,
                              level >= talent_req_level(static_cast<u8>(rank + 1)) ? th.text_muted : hud::kWarn,
                              ui::TextAlign::Center);
                }
            } else {
                hud::text(draw, Vec2{plus.x + 13.0f, plus.y + 6.0f}, "MAX", 11.0f, hud::kGold, ui::TextAlign::Center);
            }
            y += trow;
        }
    }

    const f32 fy = panel.y + panel.w - 34.0f;
    const char* how = role_ == PlayerRole::Mage ? "[K] / [ESC] CLOSE   -   [1]-[4] CAST AN ELEMENT   -   [CTRL] WEAVE A COMBO"
                                                : "[K] / [ESC] CLOSE   -   CLICK A LEARNED SKILL TO PUT IT ON YOUR BAR";
    hud::rich(draw, Vec2{hx, fy}, how, 12.0f, th.text_muted);
}

void ClientApp::skills_click(const Vec2& p) {
    // UPGRADE (gold rank) on the detail card first, then the talent "+" buttons, then the nodes.
    for (u8 a = 0; a < kAbilityCount; ++a) {
        if (in_rect(p, skill_upgrade_rects_[a])) {
            request_ability_upgrade(a);
            return;
        }
    }
    for (u8 t = 0; t < kTalentCount; ++t) {
        if (in_rect(p, talent_rects_[t])) {
            request_learn(static_cast<u8>(8 + t));
            return;
        }
    }
    for (u8 a = 0; a < kAbilityCount; ++a) {
        if (!in_rect(p, skill_node_rects_[a])) {
            continue;
        }
        skill_hover_ = a;
        if (in_rect(p, skill_learn_rects_[a])) {
            request_learn(static_cast<u8>(a + 1)); // learn it (the server spends the point)
        } else if (knows(known_mask(), a)) {
            equip_ability(a);
        }
        return;
    }
}

// ---- The journey in the world + the journal (J) ------------------------------------------------

void ClientApp::update_town_arrival(Timestep dt) {
    town_banner_ = std::max(0.0f, town_banner_ - dt.seconds);
    if (world_seed_ == 0 || !have_snapshot_) {
        return;
    }
    const Vec3 f = local_feet();
    // Announce a town a couple of metres inside its wall; forget it only once well outside (so
    // pacing along the gate doesn't re-trigger the banner).
    if (const auto v = worldgen::village_containing(f.x, f.z, world_seed_, -2.0f)) {
        if (v->vseed != town_vseed_) {
            town_vseed_ = v->vseed;
            town_banner_name_ = town_name(Vec3{v->center.x, 0.0f, v->center.y});
            for (char& c : town_banner_name_) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            // (Not over a scripted close-up of the fighting - it would cover the top of the shot.)
            town_banner_ = dev_screen() == "bestiary" || dev_screen() == "quest" ? 0.0f : 4.5f;
        }
    } else if (!worldgen::inside_village(f.x, f.z, world_seed_, 8.0f)) {
        town_vseed_ = 0;
    }
}

void ClientApp::draw_journey_guide(ui::DrawList& draw, f32 W, f32 H) {
    if (!have_snapshot_) {
        return;
    }
    const ui::Theme& th = ui::theme();
    // "You have arrived": the town's name in gold display capitals, fading in + out.
    if (town_banner_ > 0.0f && levelup_fx_ <= 0.0f && journey_fx_ <= 0.0f && !overlay_open()) {
        const f32 t = 4.5f - town_banner_;
        const f32 a = glm::clamp(t / 0.6f, 0.0f, 1.0f) * glm::clamp(town_banner_ / 0.9f, 0.0f, 1.0f);
        const f32 size = glm::clamp(H * 0.065f, 30.0f, 72.0f);
        const f32 y = H * 0.16f;
        ui::TextStyle st = hud::style(size, Vec4{th.title.r, th.title.g, th.title.b, a}, ui::TextAlign::Center,
                                      ui::FontFace::Display);
        st.color_bottom = Vec4{th.title_bottom.r, th.title_bottom.g, th.title_bottom.b, a};
        st.tracking = 0.08f;
        draw.text(Vec2{W * 0.5f, y}, town_banner_name_, st);
        const f32 tw = hud::heading_width(town_banner_name_, size) * 1.05f;
        const Vec4 rule{th.accent_hover.r, th.accent_hover.g, th.accent_hover.b, 0.8f * a};
        draw.line(Vec2{W * 0.5f - tw * 0.5f, y + size + 12.0f}, Vec2{W * 0.5f - 14.0f, y + size + 12.0f}, 1.5f, rule);
        draw.line(Vec2{W * 0.5f + 14.0f, y + size + 12.0f}, Vec2{W * 0.5f + tw * 0.5f, y + size + 12.0f}, 1.5f, rule);
        hud::stud(draw, Vec2{W * 0.5f, y + size + 12.0f}, 4.0f * a + 0.5f);
        const bool offering = snapshot_.contract_phase == static_cast<u8>(ContractPhase::Offer);
        const char* sub = offering ? "CONTRACTS WAIT IN THE MARKET SQUARE" : "SAFE BEHIND THE WALLS - FOR NOW";
        hud::text(draw, Vec2{W * 0.5f, y + size + 24.0f}, sub, size * 0.24f, hud::alpha(th.text, a), ui::TextAlign::Center);
    }

    // A new hero's next goal, marked in the world: the nearest contract wagon while the journey says
    // "find work" / "hit the road" (until they've picked one).
    const u8 step = live_progress_.journey;
    const bool guide = snapshot_.contract_phase == static_cast<u8>(ContractPhase::Offer) &&
                       step <= static_cast<u8>(JourneyGoal::SetOut) && selected_wagon_ == 0 && near_wagon_ == 0 &&
                       !overlay_open();
    if (!guide || snapshot_.wagons.empty()) {
        return;
    }
    const Vec3 feet = local_feet();
    const net::WagonState* best = nullptr;
    f32 bd = 1e9f;
    for (const net::WagonState& wg : snapshot_.wagons) {
        const f32 d = glm::length(wg.position - feet);
        if (d < bd) {
            bd = d;
            best = &wg;
        }
    }
    if (best == nullptr) {
        return;
    }
    const f32 bob = 0.18f * std::sin(elapsed_ * 3.2f);
    const Vec4 gold = hud::kGold;
    const Vec4 clip = camera_.view_projection() * Vec4{best->position + Vec3{0.0f, 3.5f + bob, 0.0f}, 1.0f};
    const bool front = clip.w > 0.05f;
    const Vec2 ndc = front ? Vec2{clip.x / clip.w, clip.y / clip.w} : Vec2{0.0f};
    if (front && std::abs(ndc.x) < 0.95f && std::abs(ndc.y) < 0.95f) {
        // A bobbing gold chevron over the wagon, with the goal named above it.
        const Vec2 sp{(ndc.x * 0.5f + 0.5f) * W, (ndc.y * 0.5f + 0.5f) * H};
        const f32 s = 13.0f;
        for (const auto& [wd, col] : {std::pair{8.0f, hud::kInk}, std::pair{4.5f, gold}}) {
            draw.line(Vec2{sp.x - s, sp.y - s * 0.6f}, Vec2{sp.x, sp.y + s * 0.5f}, wd, col);
            draw.line(Vec2{sp.x + s, sp.y - s * 0.6f}, Vec2{sp.x, sp.y + s * 0.5f}, wd, col);
        }
        hud::text(draw, Vec2{sp.x, sp.y - s * 2.4f}, "FIND WORK HERE", 13.0f, gold, ui::TextAlign::Center);
        return;
    }
    // Off-screen: a gold pointer at the edge of the screen toward it.
    Vec2 dir = front ? ndc : Vec2{-clip.x, -clip.y};
    if (glm::length(dir) < 1e-4f) {
        return;
    }
    dir = glm::normalize(dir);
    const f32 inset = 60.0f;
    const f32 t = std::min(std::abs(dir.x) > 1e-4f ? (W * 0.5f - inset) / std::abs(dir.x) : 1e9f,
                           std::abs(dir.y) > 1e-4f ? (H * 0.5f - inset) / std::abs(dir.y) : 1e9f);
    const Vec2 c = Vec2{W * 0.5f, H * 0.5f} + dir * t;
    const Vec2 perp{-dir.y, dir.x};
    const Vec2 tip = c + dir * 22.0f;
    for (const auto& [wd, col] : {std::pair{8.0f, hud::kInk}, std::pair{4.5f, gold}}) {
        draw.line(c - dir * 6.0f + perp * 14.0f, tip, wd, col);
        draw.line(c - dir * 6.0f - perp * 14.0f, tip, wd, col);
    }
    hud::text(draw, Vec2{c.x - dir.x * 26.0f, c.y - dir.y * 26.0f - 7.0f}, "WORK", 12.0f, gold, ui::TextAlign::Center);
}

void ClientApp::draw_journal() {
    if (renderer_ == nullptr) {
        return;
    }
    const VkExtent2D ext = renderer_->extent();
    const f32 W = static_cast<f32>(ext.width);
    const f32 H = static_cast<f32>(ext.height);
    ui::DrawList draw{*renderer_};
    const ui::Theme& th = ui::theme();
    const net::HeroProgress& pr = restore_acked_ ? live_progress_ : hero_.progress;
    const u8 level = hero_level();
    const Vec3 pc = player_color(restore_acked_ ? live_color_ : hero_.color);
    const Vec3 accent = role_color(role_);

    const f32 mg = std::min(W, H) * 0.05f;
    const Vec4 panel{mg, mg, W - 2.0f * mg, H - 2.0f * mg};
    const f32 top = hud::window(draw, W, H, panel, "JOURNEY");
    hud::text(draw, Vec2{panel.x + panel.z - 34.0f, panel.y + 34.0f}, "THE ROAD FROM GREENHORN TO LEGEND", 13.0f,
              th.text_muted, ui::TextAlign::Right);

    // ---- Left: the hero and their record.
    const f32 lx = panel.x + 34.0f;
    const f32 lw = std::min(panel.z * 0.32f, 330.0f);
    {
        const f32 cr = 30.0f;
        const Vec2 cc{lx + cr, top + cr + 4.0f};
        hud::medallion(draw, cc, cr, Vec4{0.07f, 0.05f, 0.035f, 1.0f});
        draw.glow(Vec4{cc.x - cr * 1.7f, cc.y - cr * 1.7f, cr * 3.4f, cr * 3.4f}, Vec4{accent, 0.25f}, Vec4{accent, 0.0f});
        draw_ability_icon(draw, role_, 0, cc.x, cc.y, cr * 0.5f, Vec4{accent, 1.0f});
        hud::text(draw, Vec2{cc.x + cr + 16.0f, top + 6.0f}, hero_.name, 24.0f, Vec4{glm::mix(pc, Vec3{1.0f}, 0.35f), 1.0f},
                  ui::TextAlign::Left, ui::FontFace::Display);
        hud::text(draw, Vec2{cc.x + cr + 16.0f, top + 38.0f},
                  std::format("LEVEL {} {}  -  {}", level, role_name(role_), race_name(appearance_.race)), 12.5f, th.text);
        f32 y = top + 2.0f * cr + 26.0f;
        hud::bar(draw, Vec4{lx, y, lw, 13.0f}, level_progress(pr.xp), Vec4{1.0f, 0.9f, 0.55f, 1.0f},
                 Vec4{0.78f, 0.55f, 0.2f, 1.0f}, 10);
        y += 20.0f;
        hud::text(draw, Vec2{lx, y}, level >= kMaxLevel ? std::string{"MAX LEVEL"}
                                                        : std::format("{} XP TO LEVEL {}", xp_to_reach(static_cast<u8>(level + 1)) - pr.xp, level + 1),
                  11.5f, th.text_muted);
        y += 34.0f;
        const u32 secs = hero_.played_seconds + static_cast<u32>(session_time_);
        const std::pair<std::string, std::string> stats[] = {
            {"RAIDERS FELLED", std::format("{}", pr.kills)},
            {"WAGONS DELIVERED", std::format("{}", pr.deliveries)},
            {"DEADLIEST ROAD", pr.best_danger == 0 ? std::string{"-"} : std::format("DANGER {}", pr.best_danger)},
            {"GEAR", tier_name(static_cast<EquipmentTier>(std::min<u8>(pr.owned_tier, 3)))},
            {"SKILLS KNOWN", std::format("{} / {}", std::popcount(static_cast<unsigned>(known_mask())), kAbilityCount)},
            {"ON THE ROAD", std::format("{}H {:02}M", secs / 3600u, (secs / 60u) % 60u)},
        };
        for (const auto& [k, v] : stats) {
            draw.line(Vec2{lx, y + 22.0f}, Vec2{lx + lw, y + 22.0f}, 1.0f, hud::alpha(th.accent, 0.18f));
            hud::text(draw, Vec2{lx, y}, k, 13.0f, th.text_muted);
            hud::text(draw, Vec2{lx + lw, y}, v, 14.0f, th.text, ui::TextAlign::Right);
            y += 32.0f;
        }
    }

    // ---- Right: every goal on the journey, in order.
    const f32 rx = lx + lw + 44.0f;
    const f32 rw = panel.x + panel.z - 34.0f - rx;
    const f32 bottom = panel.y + panel.w - 44.0f;
    const f32 row_h = std::min(56.0f, (bottom - top) / static_cast<f32>(kJourneySteps));
    HeroRecord rec;
    rec.kills = pr.kills;
    const u8 cur = std::min<u8>(pr.journey, kJourneySteps);
    for (u8 s = 0; s < kJourneySteps; ++s) {
        const JourneyStep js = journey_step(s);
        const f32 y = top + static_cast<f32>(s) * row_h;
        const bool done = s < cur;
        const bool now = s == cur;
        const Vec4 card{rx, y + 2.0f, rw, row_h - 6.0f};
        if (now) {
            const f32 pulse = 0.5f + 0.5f * std::sin(elapsed_ * 3.0f);
            draw.shadow(card, 8.0f, 10.0f, hud::alpha(hud::kGold, 0.18f + 0.15f * pulse));
        }
        draw.gradient(card, Vec4{1.0f, 0.9f, 0.7f, now ? 0.10f : 0.035f}, Vec4{1.0f, 0.9f, 0.7f, now ? 0.04f : 0.01f}, 8.0f,
                      hud::alpha(th.accent, now ? 0.7f : 0.18f), now ? 1.5f : 1.0f);
        // Status mark: a gold tick (done), a glowing diamond (now), an empty ring (ahead).
        const Vec2 mc{card.x + 22.0f, card.y + card.w * 0.5f};
        if (done) {
            draw.line(Vec2{mc.x - 8.0f, mc.y}, Vec2{mc.x - 2.0f, mc.y + 7.0f}, 4.0f, hud::kGood);
            draw.line(Vec2{mc.x - 2.0f, mc.y + 7.0f}, Vec2{mc.x + 10.0f, mc.y - 8.0f}, 4.0f, hud::kGood);
        } else if (now) {
            const f32 r = 8.0f;
            draw.line(Vec2{mc.x, mc.y - r}, Vec2{mc.x + r, mc.y}, 3.0f, hud::kGold);
            draw.line(Vec2{mc.x + r, mc.y}, Vec2{mc.x, mc.y + r}, 3.0f, hud::kGold);
            draw.line(Vec2{mc.x, mc.y + r}, Vec2{mc.x - r, mc.y}, 3.0f, hud::kGold);
            draw.line(Vec2{mc.x - r, mc.y}, Vec2{mc.x, mc.y - r}, 3.0f, hud::kGold);
            draw.rect(Vec4{mc.x - 3.0f, mc.y - 3.0f, 6.0f, 6.0f}, hud::kGold, 3.0f);
        } else {
            draw.outline(Vec4{mc.x - 7.0f, mc.y - 7.0f, 14.0f, 14.0f}, hud::alpha(th.text_muted, 0.6f), 1.5f, 7.0f);
        }
        const f32 tx = card.x + 46.0f;
        const f32 ts = std::min(row_h * 0.3f, 16.0f);
        std::string title = std::format("{}.  {}", s + 1, js.title);
        if (now && js.target > 0) {
            title += std::format("   {} / {}", journey_count(s, rec), js.target);
        }
        hud::text(draw, Vec2{tx, card.y + card.w * 0.5f - ts - 1.0f}, title, ts,
                  done ? hud::alpha(th.text, 0.6f) : (now ? hud::kGold : th.text), ui::TextAlign::Left, ui::FontFace::Display);
        hud::rich(draw, Vec2{tx, card.y + card.w * 0.5f + 3.0f}, js.hint, ts * 0.66f,
                  done ? hud::alpha(th.text_muted, 0.6f) : th.text_muted);
        const std::string reward = std::format("+{} XP", js.xp);
        hud::chip(draw, Vec2{card.x + card.z - hud::width(reward, 11.0f) - 26.0f, card.y + (card.w - 18.7f) * 0.5f}, reward,
                  11.0f, done ? hud::alpha(hud::kGood, 0.7f) : hud::kGold);
    }
    if (cur >= kJourneySteps) {
        hud::text(draw, Vec2{rx, bottom + 6.0f}, "EVERY GOAL MET - THE ROAD GOES ON", 13.0f, hud::kGood);
    }
    hud::rich(draw, Vec2{lx, panel.y + panel.w - 34.0f}, "[J] / [ESC] CLOSE", 12.0f, th.text_muted);
}

} // namespace alryn::game
