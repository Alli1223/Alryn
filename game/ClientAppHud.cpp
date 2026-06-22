// ClientApp - in-game HUD, contract panel, ability bar, world map and health bars.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

namespace alryn::game {

void ClientApp::draw_health_bars() {
    if (renderer_ == nullptr || !have_snapshot_) {
        return;
    }
    const VkExtent2D ext = renderer_->extent();
    const f32 W = static_cast<f32>(ext.width);
    const f32 H = static_cast<f32>(ext.height);
    const Mat4 vp = camera_.view_projection();
    auto bar = [&](const Vec3& world, f32 frac, const Vec3& col) {
        const Vec4 clip = vp * Vec4{world, 1.0f};
        if (clip.w <= 0.05f) {
            return; // behind the camera
        }
        const Vec2 ndc{clip.x / clip.w, clip.y / clip.w};
        if (std::abs(ndc.x) > 1.15f || std::abs(ndc.y) > 1.15f) {
            return; // off-screen
        }
        const f32 sx = (ndc.x * 0.5f + 0.5f) * W;
        const f32 sy = (ndc.y * 0.5f + 0.5f) * H;
        const f32 bw = glm::clamp(300.0f / clip.w, 16.0f, 48.0f);
        const f32 bh = 4.5f;
        frac = glm::clamp(frac, 0.0f, 1.0f);
        renderer_->draw_ui_rect(Vec4{sx - bw * 0.5f - 1.0f, sy - 1.0f, bw + 2.0f, bh + 2.0f},
                                Vec4{0.04f, 0.04f, 0.05f, 0.65f}, 1.5f);
        renderer_->draw_ui_rect(Vec4{sx - bw * 0.5f, sy, bw * frac, bh}, Vec4{col, 0.95f}, 1.0f);
    };
    for (const net::EnemyState& en : snapshot_.enemies) {
        const f32 hgt = en.kind == 2 ? 3.0f : 2.2f;
        bar(en.position + Vec3{0.0f, hgt, 0.0f}, static_cast<f32>(en.health) / 255.0f,
            Vec3{0.92f, 0.26f, 0.2f});
    }
    for (const net::VillagerState& vl : snapshot_.villagers) {
        if (vl.kind == 0 && vl.health >= 250) {
            continue; // hide bars over healthy villagers (only show the hurt)
        }
        const Vec3 col = vl.kind == 1 ? Vec3{0.45f, 0.72f, 0.96f} : Vec3{0.42f, 0.86f, 0.42f};
        bar(vl.position + Vec3{0.0f, 2.15f, 0.0f}, static_cast<f32>(vl.health) / 255.0f, col);
    }
}

bool ClientApp::world_to_screen(const Vec3& world, f32 W, f32 H, Vec2& out) const {
    const Vec4 clip = camera_.view_projection() * Vec4{world, 1.0f};
    if (clip.w <= 0.05f) {
        return false;
    }
    const Vec2 ndc{clip.x / clip.w, clip.y / clip.w};
    out = Vec2{(ndc.x * 0.5f + 0.5f) * W, (ndc.y * 0.5f + 0.5f) * H};
    return std::abs(ndc.x) < 1.3f && std::abs(ndc.y) < 1.3f;
}

void ClientApp::draw_hud() {
    if (renderer_ == nullptr || !have_snapshot_) {
        return;
    }
    f32 hp = 1.0f;
    for (const net::PlayerState& p : snapshot_.players) {
        if (p.id == my_id_) {
            hp = static_cast<f32>(p.health) / 100.0f;
            break;
        }
    }
    const VkExtent2D ext = renderer_->extent();
    const f32 W = static_cast<f32>(ext.width);
    const f32 H = static_cast<f32>(ext.height);
    ui::DrawList draw{*renderer_};
    const f32 ts = glm::clamp(H * 0.026f, 15.0f, 30.0f);
    const Vec3 feet = local_feet();
    const u8 phase = snapshot_.contract_phase;

    // Shared party money, top-right.
    const std::string money = std::format("$ {}", snapshot_.money);
    draw.text(Vec2{W - draw.text_width(money, ts) - 24.0f, 22.0f}, money, ts,
              Vec4{0.96f, 0.86f, 0.4f, 1.0f});

    if (phase == static_cast<u8>(ContractPhase::Offer)) {
        draw.text(Vec2{24.0f, 22.0f}, "WALK UP TO A WAGON TO VIEW ITS CONTRACT", ts,
                  Vec4{0.94f, 0.86f, 0.58f, 1.0f});
        // A small floating tag over every offered wagon so you can see where they are and
        // pick which to walk to (gold $ reward; the one you're next to is highlighted).
        for (const net::WagonState& wg : snapshot_.wagons) {
            if (wg.id == selected_wagon_ || wg.id == near_wagon_) {
                continue; // the focused wagon gets the full panel instead
            }
            Vec2 sp;
            if (world_to_screen(wg.position + Vec3{0.0f, 2.6f, 0.0f}, W, H, sp)) {
                const std::string tag = std::format("$ {}", wg.reward);
                draw.text(Vec2{sp.x - draw.text_width(tag, ts * 0.7f) * 0.5f, sp.y}, tag, ts * 0.7f,
                          Vec4{0.9f, 0.82f, 0.45f, 0.95f});
            }
        }
        // The full contract panel for the focused wagon (the accepted one, else the one in range).
        accept_btn_ = cancel_btn_ = ui::Rect{};
        panel_wagon_ = selected_wagon_ != 0 ? selected_wagon_ : near_wagon_;
        if (const net::WagonState* wg = wagon_by_id(panel_wagon_)) {
            draw_contract_panel(draw, *wg, selected_wagon_ == panel_wagon_, W, H, ts, feet);
        }
    } else if (phase == static_cast<u8>(ContractPhase::Active) && !snapshot_.wagons.empty()) {
        const net::WagonState& wg = snapshot_.wagons.front();
        const VehicleType& vt = vehicle_type(wg.type);
        const f32 dist = glm::length(Vec2{wg.dest.x - feet.x, wg.dest.z - feet.z});
        const bool manual = wg.mode == static_cast<u8>(WagonMode::Manual);
        std::string title = vt.name();
        for (char& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        draw.text(Vec2{24.0f, 22.0f},
                  std::format("DELIVER THE {}   $ {}{}   ~{}m", title, wg.reward,
                              manual ? " (manual)" : "", static_cast<int>(dist)),
                  ts, Vec4{0.94f, 0.86f, 0.58f, 1.0f});
        // Cargo load (pay scales with the share delivered).
        const bool short_load = wg.goods_aboard < wg.goods_total;
        draw.text(Vec2{24.0f, 22.0f + ts * 1.5f},
                  std::format("GOODS {}/{}", wg.goods_aboard, wg.goods_total), ts * 0.72f,
                  short_load ? Vec4{0.95f, 0.7f, 0.35f, 1.0f} : Vec4{0.66f, 0.78f, 0.7f, 1.0f});
        // Wagon health bar.
        const f32 wf = static_cast<f32>(wg.health) / 255.0f;
        draw.rect(Vec4{24.0f, 22.0f + ts * 2.6f, 220.0f, 12.0f}, Vec4{0.05f, 0.05f, 0.07f, 0.7f},
                  3.0f);
        draw.rect(Vec4{24.0f, 22.0f + ts * 2.6f, 220.0f * std::max(wf, 0.0f), 12.0f},
                  Vec4{glm::mix(Vec3{0.8f, 0.3f, 0.2f}, Vec3{0.5f, 0.7f, 0.4f}, wf), 0.95f}, 3.0f);
        // Contextual E hint: righting a flipped cart / handling goods take priority.
        bool carrying = false;
        for (const net::PlayerState& pp : snapshot_.players) {
            if (pp.id == my_id_) {
                carrying = pp.carrying != 0;
            }
        }
        bool near_good = false;
        for (const net::GoodState& g : snapshot_.goods) {
            if (g.loose != 0 && glm::length(g.position - feet) < kGoodPickupRange + 0.5f) {
                near_good = true;
            }
        }
        std::string hint;
        if (carrying) {
            hint = "[E] load the crate into the cart";
        } else if (near_good) {
            hint = "[E] pick up the fallen crate";
        } else if (!manual) {
            hint = "[E] ride on the wagon";
        } else if (vt.horse_drawn()) {
            hint = "[E] drive (W/S throttle, A/D rein) / ride";
        } else {
            hint = "[E] haul / ride the wagon";
        }
        draw.text(Vec2{24.0f, 22.0f + ts * 4.0f}, hint, ts * 0.72f, Vec4{0.66f, 0.78f, 0.7f, 1.0f});
        // Warn when crates have bounced out and aren't recovered (pay is dropping).
        if (short_load) {
            const char* warn = "CARGO SPILLING - RECOVER THE CRATES";
            draw.text(Vec2{(W - draw.text_width(warn, ts * 0.9f)) * 0.5f, H * 0.3f}, warn,
                      ts * 0.9f, Vec4{0.95f, 0.6f, 0.3f, 1.0f});
        }
        draw_dest_arrow(draw, feet, Vec3{wg.dest.x, feet.y, wg.dest.z}, W);
    }

    // Settle banner.
    if (snapshot_.contract_outcome != 0) {
        const bool ok = snapshot_.contract_outcome == 1;
        const char* msg = ok ? "WAGON DELIVERED" : "WAGON LOST";
        const Vec4 c = ok ? Vec4{0.55f, 0.9f, 0.55f, 1.0f} : Vec4{0.95f, 0.4f, 0.36f, 1.0f};
        const f32 bs = glm::clamp(H * 0.06f, 28.0f, 76.0f);
        draw.text(Vec2{(W - draw.text_width(msg, bs)) * 0.5f, H * 0.34f}, msg, bs, c);
    }

    draw.text(Vec2{24.0f, H - 52.0f}, "[M] MAP    [K] SKILLS", ts * 0.72f,
              Vec4{0.72f, 0.80f, 0.88f, 1.0f});

    // Health bar.
    const f32 bw = std::min(320.0f, W * 0.28f);
    const f32 bh = 18.0f;
    const f32 x = 24.0f;
    const f32 y = H - 24.0f - bh;
    draw.rect(Vec4{x - 3.0f, y - 3.0f, bw + 6.0f, bh + 6.0f}, Vec4{0.05f, 0.05f, 0.07f, 0.7f},
              5.0f);
    const Vec3 col = glm::mix(Vec3{0.85f, 0.2f, 0.18f}, Vec3{0.35f, 0.8f, 0.35f}, hp);
    draw.rect(Vec4{x, y, bw * std::max(hp, 0.0f), bh}, Vec4{col, 0.95f}, 4.0f);
    // Role label above the health bar.
    draw.text(Vec2{x, y - ts * 0.95f}, role_name(role_), ts * 0.72f,
              Vec4{0.74f, 0.82f, 0.92f, 1.0f});

    // Cleric heal-channel charge bar (centre screen while charging the AOE heal).
    if (role_ == PlayerRole::Cleric && heal_charge_fx_ > 0.001f) {
        const f32 frac = glm::clamp(heal_charge_fx_ / kHealChargeTime, 0.0f, 1.0f);
        const f32 cw = std::min(360.0f, W * 0.32f);
        const f32 cx = (W - cw) * 0.5f;
        const f32 cy = H * 0.62f;
        draw.rect(Vec4{cx - 3.0f, cy - 3.0f, cw + 6.0f, 18.0f + 6.0f},
                  Vec4{0.04f, 0.06f, 0.05f, 0.8f}, 5.0f);
        draw.rect(Vec4{cx, cy, cw * frac, 18.0f}, Vec4{0.45f, 1.0f, 0.7f, 0.95f}, 4.0f);
        draw.text(Vec2{cx, cy - ts * 0.95f}, "CHANNELLING HEAL...", ts * 0.7f,
                  Vec4{0.7f, 1.0f, 0.85f, 1.0f});
    }

    draw_ability_bar(draw, W, H, ts);
}

void ClientApp::draw_contract_panel(ui::DrawList& draw, const net::WagonState& wg, bool accepted, f32 W, f32 H, f32 ts, const Vec3& feet) {
    const f32 pw = glm::clamp(W * 0.24f, 280.0f, 380.0f);
    const f32 ph = ts * 9.6f;
    // Anchor beside the wagon on screen; clamp on-screen, fall back to centre if off-camera.
    Vec2 sp;
    Vec2 anchor{(W - pw) * 0.5f, H * 0.26f};
    if (world_to_screen(wg.position + Vec3{0.0f, 2.6f, 0.0f}, W, H, sp)) {
        anchor = Vec2{sp.x - pw * 0.5f, sp.y - ph - 12.0f};
    }
    anchor.x = glm::clamp(anchor.x, 12.0f, W - pw - 12.0f);
    anchor.y = glm::clamp(anchor.y, 12.0f, H - ph - 120.0f);
    const f32 px = anchor.x, py = anchor.y;

    const Vec3 accent{0.96f, 0.86f, 0.45f};
    draw.rect(Vec4{px, py, pw, ph}, Vec4{0.05f, 0.06f, 0.09f, 0.92f}, Vec4{accent, 0.85f}, 2.0f,
              10.0f);
    const f32 ix = px + 18.0f;
    f32 iy = py + 16.0f;

    // Heading: bound-for town name.
    draw.text(Vec2{ix, iy}, "CARGO CONTRACT", ts * 0.62f, Vec4{0.7f, 0.75f, 0.82f, 1.0f});
    iy += ts * 1.2f;
    draw.text(Vec2{ix, iy}, std::format("TO {}", town_name(Vec3{wg.dest.x, 0.0f, wg.dest.z})),
              ts * 1.05f, Vec4{0.98f, 0.92f, 0.7f, 1.0f});
    iy += ts * 1.7f;

    // Distance + danger + pay.
    const f32 dist = glm::length(Vec2{wg.dest.x - feet.x, wg.dest.z - feet.z});
    draw.text(Vec2{ix, iy}, std::format("DISTANCE   ~{} m", static_cast<int>(dist)), ts * 0.8f,
              Vec4{0.82f, 0.85f, 0.9f, 1.0f});
    iy += ts * 1.25f;
    const char* danger = wg.difficulty <= 1 ? "LOW" : wg.difficulty == 2 ? "MODERATE" : "HIGH";
    const Vec4 dcol = wg.difficulty <= 1 ? Vec4{0.55f, 0.85f, 0.55f, 1.0f}
                      : wg.difficulty == 2 ? Vec4{0.95f, 0.8f, 0.4f, 1.0f}
                                           : Vec4{0.95f, 0.45f, 0.4f, 1.0f};
    draw.text(Vec2{ix, iy}, std::format("DANGER     {} {}", danger,
                                        std::string(wg.difficulty, '*')),
              ts * 0.8f, dcol);
    iy += ts * 1.25f;
    draw.text(Vec2{ix, iy}, std::format("PAY        $ {}", wg.reward), ts * 0.9f,
              Vec4{0.98f, 0.88f, 0.42f, 1.0f});

    // Mode hint (toggled with H).
    const char* mode = vote_mode_ == 2 ? "HAUL MANUALLY (+pay)" : "HIRE A DRIVER";
    draw.text(Vec2{ix, py + ph - ts * 2.9f}, std::format("[H] {}", mode), ts * 0.66f,
              Vec4{0.66f, 0.78f, 0.7f, 1.0f});

    // Buttons.
    const f32 bw = (pw - 18.0f * 2.0f - 12.0f) * 0.5f;
    const f32 bh = ts * 1.7f;
    const f32 by = py + ph - bh - 14.0f;
    if (accepted) {
        const int total = static_cast<int>(snapshot_.players.size());
        draw.rect(Vec4{ix, by, bw, bh}, Vec4{0.12f, 0.16f, 0.13f, 0.95f}, 6.0f);
        draw.text(Vec2{ix + 10.0f, by + bh * 0.5f - ts * 0.34f},
                  std::format("WAITING {}/{}", wg.votes, total), ts * 0.62f,
                  Vec4{0.7f, 0.9f, 0.7f, 1.0f});
        cancel_btn_ = ui::Rect{ix + bw + 12.0f, by, bw, bh};
        draw.rect(Vec4{cancel_btn_.x, cancel_btn_.y, bw, bh}, Vec4{0.22f, 0.12f, 0.12f, 0.95f},
                  Vec4{0.8f, 0.4f, 0.4f, 0.8f}, 1.5f, 6.0f);
        draw.text(Vec2{cancel_btn_.x + bw * 0.5f - draw.text_width("CANCEL", ts * 0.7f) * 0.5f,
                       by + bh * 0.5f - ts * 0.36f},
                  "CANCEL", ts * 0.7f, Vec4{0.95f, 0.8f, 0.8f, 1.0f});
    } else {
        accept_btn_ = ui::Rect{ix, by, bw, bh};
        draw.rect(Vec4{accept_btn_.x, accept_btn_.y, bw, bh}, Vec4{0.14f, 0.3f, 0.16f, 0.97f},
                  Vec4{0.5f, 0.9f, 0.5f, 0.95f}, 2.0f, 6.0f);
        draw.text(Vec2{accept_btn_.x + bw * 0.5f - draw.text_width("ACCEPT", ts * 0.78f) * 0.5f,
                       by + bh * 0.5f - ts * 0.4f},
                  "ACCEPT", ts * 0.78f, Vec4{0.85f, 1.0f, 0.85f, 1.0f});
        cancel_btn_ = ui::Rect{ix + bw + 12.0f, by, bw, bh};
        draw.rect(Vec4{cancel_btn_.x, cancel_btn_.y, bw, bh}, Vec4{0.16f, 0.17f, 0.2f, 0.95f},
                  Vec4{0.6f, 0.62f, 0.66f, 0.8f}, 1.5f, 6.0f);
        draw.text(Vec2{cancel_btn_.x + bw * 0.5f - draw.text_width("CANCEL", ts * 0.7f) * 0.5f,
                       by + bh * 0.5f - ts * 0.36f},
                  "CANCEL", ts * 0.7f, Vec4{0.85f, 0.87f, 0.9f, 1.0f});
    }
}

Vec3 ClientApp::role_color(PlayerRole role) {
    switch (role) {
        case PlayerRole::Knight: return Vec3{0.56f, 0.7f, 0.96f};  // steel blue
        case PlayerRole::Hunter: return Vec3{0.55f, 0.92f, 0.55f}; // forest green
        case PlayerRole::Cleric: return Vec3{0.97f, 0.86f, 0.46f}; // holy gold
    }
    return Vec3{1.0f};
}

void ClientApp::draw_ability_bar(ui::DrawList& draw, f32 W, f32 H, f32 ts) {
    const Vec3 accent = role_color(role_);
    const f32 slot = glm::clamp(H * 0.092f, 56.0f, 84.0f);
    const f32 gap = slot * 0.16f;
    const f32 pad = slot * 0.16f;
    const f32 total = slot * kAbilitySlots + gap * (kAbilitySlots - 1);
    const f32 sy = H - slot - pad - 26.0f;
    const f32 x0 = (W - total) * 0.5f;

    // Backing panel.
    draw.rect(Vec4{x0 - pad, sy - pad, total + pad * 2.0f, slot + pad * 2.0f},
              Vec4{0.04f, 0.05f, 0.07f, 0.72f}, 10.0f);

    const f32 r = slot * 0.16f;
    f32 sx = x0;
    for (u8 i = 0; i < kAbilitySlots; ++i) {
        ability_slot_rects_[i] = ui::Rect{sx, sy, slot, slot}; // cached for drag hit-testing
        const int a = bar_[i];
        const bool empty = a < 0;
        const bool dragging = drag_slot_ == static_cast<int>(i);

        if (empty) {
            // An empty slot: a dim dashed-looking frame + a faint key badge (drop a skill here).
            draw.rect(Vec4{sx, sy, slot, slot}, Vec4{0.07f, 0.08f, 0.10f, 0.7f},
                      Vec4{accent, 0.18f}, 1.5f, r);
            draw.text(Vec2{sx + 4.0f + slot * 0.08f, sy + 4.0f + slot * 0.04f},
                      std::format("{}", i + 1), slot * 0.18f, Vec4{accent, 0.4f});
            sx += slot + gap;
            continue;
        }

        const AbilityDef ab = ability_def(role_, static_cast<u8>(a));
        const f32 frac =
            ab.cooldown > 0.0f ? glm::clamp(ability_cd_[a] / ab.cooldown, 0.0f, 1.0f) : 0.0f;
        const bool ready = frac <= 0.0f;

        // Slot face + an accent border that lights up when the ability is ready.
        draw.rect(Vec4{sx, sy, slot, slot}, Vec4{0.11f, 0.13f, 0.17f, 0.95f},
                  Vec4{accent, ready ? 0.95f : 0.28f}, ready ? 2.5f : 1.5f, r);

        // Icon, tinted by readiness (centred - the bar is icon-only; names live in the skills tree).
        const Vec4 icol{ready ? accent : accent * 0.45f, ready ? 1.0f : 0.7f};
        draw_ability_icon(draw, role_, static_cast<u8>(a), sx + slot * 0.5f, sy + slot * 0.5f,
                          slot * 0.26f, icol);

        // Cooldown wipe: a dark overlay draining from the top + the seconds remaining.
        if (!ready) {
            draw.rect(Vec4{sx, sy, slot, slot * frac}, Vec4{0.02f, 0.02f, 0.03f, 0.62f}, r);
            const std::string secs = std::format("{:.0f}", std::ceil(ability_cd_[a]));
            draw.text(Vec2{sx + slot * 0.5f - draw.text_width(secs, ts) * 0.5f,
                           sy + slot * 0.5f - ts * 0.5f},
                      secs, ts, Vec4{0.95f, 0.96f, 1.0f, 0.95f});
        }
        // The slot being click-dragged is dimmed; its icon follows the cursor (drawn below).
        if (dragging) {
            draw.rect(Vec4{sx, sy, slot, slot}, Vec4{0.03f, 0.04f, 0.05f, 0.6f}, r);
        }

        // Key badge (top-left).
        draw.rect(Vec4{sx + 4.0f, sy + 4.0f, slot * 0.26f, slot * 0.26f},
                  Vec4{accent, 0.9f}, slot * 0.07f);
        draw.text(Vec2{sx + 4.0f + slot * 0.08f, sy + 4.0f + slot * 0.04f},
                  std::format("{}", i + 1), slot * 0.18f, Vec4{0.05f, 0.06f, 0.08f, 1.0f});
        sx += slot + gap;
    }

    // The dragged ability rides under the cursor while reordering.
    if (drag_slot_ >= 0 && bar_[drag_slot_] >= 0) {
        const Vec2 p = pointer_pos();
        draw.rect(Vec4{p.x - slot * 0.5f, p.y - slot * 0.5f, slot, slot},
                  Vec4{0.11f, 0.13f, 0.17f, 0.85f}, Vec4{accent, 0.95f}, 2.0f, r);
        draw_ability_icon(draw, role_, static_cast<u8>(bar_[drag_slot_]), p.x, p.y, slot * 0.26f,
                          Vec4{accent, 1.0f});
    }
}

void ClientApp::draw_ability_icon(ui::DrawList& draw, PlayerRole role, u8 slot, f32 cx, f32 cy, f32 r, const Vec4& c) {
    const f32 th = r * 0.34f;
    auto L = [&](Vec2 a, Vec2 b) { draw.line(Vec2{cx, cy} + a, Vec2{cx, cy} + b, th, c); };
    auto arrow = [&](Vec2 tail, Vec2 tip) { // a line with a two-stroke head
        draw.line(Vec2{cx, cy} + tail, Vec2{cx, cy} + tip, th * 0.8f, c);
        const Vec2 d = glm::normalize(tip - tail) * (r * 0.45f);
        const Vec2 p{-d.y, d.x};
        draw.line(Vec2{cx, cy} + tip, Vec2{cx, cy} + tip - d + p * 0.6f, th * 0.8f, c);
        draw.line(Vec2{cx, cy} + tip, Vec2{cx, cy} + tip - d - p * 0.6f, th * 0.8f, c);
    };
    switch (role) {
        case PlayerRole::Knight:
            if (slot == 0) { // sword
                L(Vec2{0.0f, -r}, Vec2{0.0f, r * 0.55f});
                L(Vec2{-r * 0.5f, r * 0.35f}, Vec2{r * 0.5f, r * 0.35f}); // crossguard
                L(Vec2{0.0f, r * 0.55f}, Vec2{0.0f, r});                  // grip
            } else if (slot == 1) { // shield
                draw.rect(Vec4{cx - r * 0.7f, cy - r * 0.8f, r * 1.4f, r * 1.1f},
                          Vec4{0.0f, 0.0f, 0.0f, 0.0f}, c, th * 0.8f, r * 0.25f);
                L(Vec2{-r * 0.7f, r * 0.28f}, Vec2{0.0f, r});             // taper to a point
                L(Vec2{r * 0.7f, r * 0.28f}, Vec2{0.0f, r});
            } else if (slot == 2) { // consecration: a flame over a ground line
                L(Vec2{-r, r}, Vec2{r, r}); // ground
                L(Vec2{0.0f, r}, Vec2{-r * 0.35f, -r * 0.2f}); // flame left edge
                L(Vec2{0.0f, r}, Vec2{r * 0.35f, -r * 0.2f});  // flame right edge
                L(Vec2{-r * 0.35f, -r * 0.2f}, Vec2{0.0f, -r}); // tip
                L(Vec2{r * 0.35f, -r * 0.2f}, Vec2{0.0f, -r});
            } else if (slot == 3) { // taunt: shout waves
                for (int k = 0; k < 3; ++k) {
                    const f32 o = r * (0.1f + 0.4f * static_cast<f32>(k));
                    L(Vec2{o, -r * 0.6f}, Vec2{o + r * 0.35f, 0.0f});
                    L(Vec2{o + r * 0.35f, 0.0f}, Vec2{o, r * 0.6f});
                }
            } else if (slot == 4) { // whirlwind: a spinning four-blade pinwheel
                for (int k = 0; k < 4; ++k) {
                    const f32 ang = TwoPi * static_cast<f32>(k) / 4.0f;
                    const Vec2 d{std::cos(ang), std::sin(ang)};
                    const Vec2 t{-d.y, d.x};
                    L(d * (r * 0.2f), d * r);
                    L(d * r, d * r + t * (r * 0.45f));
                }
            } else { // rally: a banner raised on a pole
                L(Vec2{-r * 0.45f, -r}, Vec2{-r * 0.45f, r});
                L(Vec2{-r * 0.45f, -r}, Vec2{r * 0.65f, -r * 0.7f});
                L(Vec2{r * 0.65f, -r * 0.7f}, Vec2{-r * 0.45f, -r * 0.4f});
            }
            break;
        case PlayerRole::Hunter:
            if (slot == 0) { // single arrow
                arrow(Vec2{-r * 0.8f, r * 0.8f}, Vec2{r * 0.8f, -r * 0.8f});
            } else if (slot == 1) { // volley: three arrows
                for (int k = -1; k <= 1; ++k) {
                    const f32 o = static_cast<f32>(k) * r * 0.55f;
                    arrow(Vec2{-r * 0.7f + o, r * 0.7f}, Vec2{r * 0.7f + o, -r * 0.7f});
                }
            } else if (slot == 2) { // dash: three forward chevrons
                for (int k = 0; k < 3; ++k) {
                    const f32 o = -r * 0.7f + static_cast<f32>(k) * r * 0.6f;
                    L(Vec2{o, -r * 0.7f}, Vec2{o + r * 0.5f, 0.0f});
                    L(Vec2{o + r * 0.5f, 0.0f}, Vec2{o, r * 0.7f});
                }
            } else if (slot == 3) { // piercing shot: one long bold arrow
                arrow(Vec2{-r, r * 0.55f}, Vec2{r, -r * 0.55f});
                L(Vec2{-r * 0.5f, -r * 0.55f}, Vec2{-r * 0.5f, r * 0.55f}); // bowstring hint
            } else if (slot == 4) { // multishot: a fan of arrows from one nock
                const Vec2 nock{0.0f, r};
                for (int k = -2; k <= 2; ++k) {
                    const f32 a = -1.5707963f + static_cast<f32>(k) * 0.34f; // spread upward
                    const Vec2 tip{std::cos(a) * r, std::sin(a) * r};
                    L(nock, tip);
                    const Vec2 d = glm::normalize(tip - nock) * (r * 0.32f);
                    const Vec2 pp{-d.y, d.x};
                    L(tip, tip - d + pp * 0.5f);
                    L(tip, tip - d - pp * 0.5f);
                }
            } else { // caltrops: a three-spoke jack of spikes
                for (int k = 0; k < 3; ++k) {
                    const f32 a = TwoPi * static_cast<f32>(k) / 6.0f;
                    const Vec2 d{std::cos(a), std::sin(a)};
                    L(d * -r, d * r);
                }
            }
            break;
        case PlayerRole::Cleric:
            if (slot == 0 || slot == 1) { // healing cross (sanctuary adds rays)
                draw.rect(Vec4{cx - r * 0.24f, cy - r * 0.85f, r * 0.48f, r * 1.7f}, c, r * 0.1f);
                draw.rect(Vec4{cx - r * 0.85f, cy - r * 0.24f, r * 1.7f, r * 0.48f}, c, r * 0.1f);
                if (slot == 1) {
                    for (int k = 0; k < 4; ++k) {
                        const f32 a = TwoPi * (static_cast<f32>(k) + 0.5f) / 4.0f;
                        const Vec2 d{std::cos(a), std::sin(a)};
                        L(d * r * 0.7f, d * r * 1.05f);
                    }
                }
            } else if (slot == 2) { // smite: a lightning bolt
                L(Vec2{r * 0.3f, -r}, Vec2{-r * 0.35f, 0.0f});
                L(Vec2{-r * 0.35f, 0.0f}, Vec2{r * 0.2f, 0.0f});
                L(Vec2{r * 0.2f, 0.0f}, Vec2{-r * 0.3f, r});
            } else if (slot == 3) { // aegis: a shield bubble (a ring with a small cross)
                draw.rect(Vec4{cx - r * 0.85f, cy - r * 0.85f, r * 1.7f, r * 1.7f},
                          Vec4{0.0f, 0.0f, 0.0f, 0.0f}, c, th * 0.7f, r * 0.85f); // ring
                L(Vec2{0.0f, -r * 0.4f}, Vec2{0.0f, r * 0.4f});
                L(Vec2{-r * 0.4f, 0.0f}, Vec2{r * 0.4f, 0.0f});
            } else if (slot == 4) { // renew: a healing cross with rising motes
                draw.rect(Vec4{cx - r * 0.2f, cy - r * 0.55f, r * 0.4f, r * 1.1f}, c, r * 0.1f);
                draw.rect(Vec4{cx - r * 0.55f, cy - r * 0.2f, r * 1.1f, r * 0.4f}, c, r * 0.1f);
                draw.rect(Vec4{cx - r * 0.95f, cy - r * 0.95f, r * 0.22f, r * 0.22f}, c, r * 0.1f);
                draw.rect(Vec4{cx + r * 0.72f, cy - r * 0.78f, r * 0.22f, r * 0.22f}, c, r * 0.1f);
            } else { // judgement: stacked downward strike chevrons
                for (int k = 0; k < 3; ++k) {
                    const f32 o = -r * 0.7f + static_cast<f32>(k) * r * 0.6f;
                    L(Vec2{-r * 0.7f, o}, Vec2{0.0f, o + r * 0.5f});
                    L(Vec2{0.0f, o + r * 0.5f}, Vec2{r * 0.7f, o});
                }
            }
            break;
    }
}

void ClientApp::draw_dest_arrow(ui::DrawList& draw, const Vec3& from, const Vec3& to, f32 W) {
    const f32 cam_yaw = radians(iso::yaw_deg);
    const Vec2 cam_fwd{-std::cos(cam_yaw), -std::sin(cam_yaw)};
    const Vec2 cam_right{std::sin(cam_yaw), -std::cos(cam_yaw)};
    const Vec2 v{to.x - from.x, to.z - from.z};
    Vec2 d{glm::dot(v, cam_right), -glm::dot(v, cam_fwd)};
    if (glm::length(d) < 1e-3f) {
        return;
    }
    d = glm::normalize(d);
    const Vec2 perp{-d.y, d.x};
    const Vec2 c{W * 0.5f, 84.0f};
    const Vec4 amber{0.98f, 0.78f, 0.32f, 1.0f};
    const f32 L = 34.0f, hw = 13.0f;
    const Vec2 tip = c + d * L;
    draw.line(c - d * L * 0.6f, tip, 6.0f, amber);
    draw.line(tip, tip - d * hw + perp * hw, 6.0f, amber);
    draw.line(tip, tip - d * hw - perp * hw, 6.0f, amber);
    draw.text(Vec2{c.x - draw.text_width("DESTINATION", 16.0f) * 0.5f, c.y + 22.0f},
              "DESTINATION", 16.0f, amber);
}

void ClientApp::draw_map() {
    if (renderer_ == nullptr) {
        return;
    }
    const VkExtent2D ext = renderer_->extent();
    const f32 W = static_cast<f32>(ext.width);
    const f32 H = static_cast<f32>(ext.height);
    ui::DrawList draw{*renderer_};

    // Dim the world, then a framed map panel.
    draw.rect(Vec4{0.0f, 0.0f, W, H}, Vec4{0.03f, 0.04f, 0.06f, 0.86f});
    const f32 mg = std::min(W, H) * map::margin_frac;
    const Vec4 panel{mg, mg, W - 2.0f * mg, H - 2.0f * mg};
    draw.rect(panel, Vec4{0.09f, 0.10f, 0.13f, 0.97f}, Vec4{0.35f, 0.34f, 0.3f, 1.0f}, 2.0f, 10.0f);
    draw.text(Vec2{panel.x + 22.0f, panel.y + 18.0f}, "WORLD MAP", 30.0f,
              Vec4{0.92f, 0.9f, 0.96f, 1.0f});

    // Map projection: centre on the player, a fixed world span across the panel.
    const f32 inner = std::min(panel.z, panel.w) * 0.5f - 28.0f;
    const f32 view_world = map::view_world; // metres from centre to edge of the shorter side
    const f32 ppm = inner / view_world;
    const Vec2 mc{panel.x + panel.z * 0.5f, panel.y + panel.w * 0.5f};
    const Vec3 feet = local_feet();
    auto to_screen = [&](f32 wx, f32 wz) {
        return Vec2{mc.x + (wx - feet.x) * ppm, mc.y + (wz - feet.z) * ppm};
    };
    auto in_panel = [&](const Vec2& p) {
        return p.x > panel.x + 4.0f && p.x < panel.x + panel.z - 4.0f &&
               p.y > panel.y + 36.0f && p.y < panel.y + panel.w - 4.0f;
    };

    // Roads between towns.
    const Vec2 origin{feet.x, feet.z};
    for (const roads::Segment& s : roads::gather(origin, view_world * 1.6f, world_seed_)) {
        const Vec2 a = to_screen(s.a.x, s.a.y);
        const Vec2 b = to_screen(s.b.x, s.b.y);
        if (in_panel(a) || in_panel(b)) {
            draw.line(a, b, 2.5f, Vec4{0.62f, 0.52f, 0.36f, 1.0f});
        }
    }

    // Towns (markers sized by town extent).
    const int reach = static_cast<int>(view_world * 1.4f / worldgen::village_cell) + 1;
    const int ccx = static_cast<int>(std::floor(feet.x / worldgen::village_cell));
    const int ccz = static_cast<int>(std::floor(feet.z / worldgen::village_cell));
    for (int dz = -reach; dz <= reach; ++dz) {
        for (int dx = -reach; dx <= reach; ++dx) {
            const auto v = worldgen::village_at(ccx + dx, ccz + dz, world_seed_);
            if (!v) {
                continue;
            }
            const Vec2 c = to_screen(v->center.x, v->center.y);
            if (!in_panel(c)) {
                continue;
            }
            const f32 r = glm::clamp(v->half * ppm, 5.0f, 26.0f);
            draw.rect(Vec4{c.x - r, c.y - r, 2.0f * r, 2.0f * r}, Vec4{0.78f, 0.7f, 0.5f, 0.95f},
                      Vec4{0.25f, 0.22f, 0.16f, 1.0f}, 1.5f, 3.0f);
        }
    }

    // The player: a bright dot with a facing tick.
    const Vec2 me = to_screen(feet.x, feet.z);
    const Vec2 dir{std::cos(face_yaw_), std::sin(face_yaw_)};
    draw.line(me, me + dir * 14.0f, 3.0f, Vec4{0.55f, 0.85f, 1.0f, 1.0f});
    draw.rect(Vec4{me.x - 5.0f, me.y - 5.0f, 10.0f, 10.0f}, Vec4{0.4f, 0.7f, 1.0f, 1.0f}, 5.0f);

    draw.text(Vec2{panel.x + 22.0f, panel.y + panel.w - 30.0f}, "M / ESC  CLOSE", 18.0f,
              Vec4{0.7f, 0.72f, 0.78f, 1.0f});
}

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

    // Dim the world, then a framed parchment panel (the medieval menu styling).
    draw.rect(Vec4{0.0f, 0.0f, W, H}, Vec4{0.03f, 0.02f, 0.015f, 0.86f});
    const f32 mg = std::min(W, H) * 0.08f;
    const Vec4 panel{mg, mg, W - 2.0f * mg, H - 2.0f * mg};
    draw.rect(panel, th.panel, Vec4{accent, 0.7f}, 2.5f, 12.0f);

    // Header: a gold title + the chosen role and its fantasy on the right.
    const f32 hx = panel.x + 30.0f;
    f32 hy = panel.y + 24.0f;
    draw.text(Vec2{hx, hy}, "SKILLS", 34.0f, th.accent_hover);
    const std::string rn = std::format("{}  -  {}", role_name(role_), role_desc(role_));
    draw.text(Vec2{panel.x + panel.z - draw.text_width(rn, 18.0f) - 30.0f, hy + 10.0f}, rn, 18.0f,
              Vec4{accent, 1.0f});
    hy += 52.0f;

    // Controls reminder (the role's primary / secondary mouse actions).
    const char* primary = role_ == PlayerRole::Knight  ? "SWORD SWING"
                          : role_ == PlayerRole::Hunter ? "LOOSE AN ARROW"
                                                        : "ARCANE BOLT";
    const char* secondary = role_ == PlayerRole::Knight  ? "RAISE SHIELD (BLOCK)"
                            : role_ == PlayerRole::Cleric ? "CHANNEL HEAL AURA"
                                                          : "BUILD TERRAIN";
    draw.text(Vec2{hx, hy}, std::format("LEFT CLICK: {}     RIGHT CLICK: {}", primary, secondary),
              15.0f, th.text_muted);
    hy += 22.0f;
    draw.text(Vec2{hx, hy},
              "CLICK A SKILL TO EQUIP / UNEQUIP  -  DRAG THE ACTION BAR TO REORDER (KEYS 1-4)",
              15.0f, Vec4{accent, 0.9f});
    hy += 22.0f;
    draw.line(Vec2{hx, hy}, Vec2{panel.x + panel.z - 30.0f, hy}, 1.5f, Vec4{accent, 0.4f});
    hy += 12.0f;

    // The tree: a role crest on the left branches to every ability node (one per row). Each node
    // is clickable to equip/unequip (skills_click hit-tests skill_node_rects_).
    const f32 rows_top = hy;
    const f32 rows_bot = panel.y + panel.w - 46.0f;
    const f32 row_h = (rows_bot - rows_top) / static_cast<f32>(kAbilityCount);
    const f32 icon_box = std::min(row_h * 0.6f, 74.0f);
    const f32 node_x = panel.x + 176.0f; // left edge of the ability icon boxes
    const f32 root_x = panel.x + 58.0f;
    const f32 root_y = (rows_top + rows_bot) * 0.5f;
    const f32 root_r = std::min(icon_box * 0.62f, 44.0f);
    const Vec4 branch{accent.r, accent.g, accent.b, 0.5f};

    // Branch connectors (drawn first, behind the nodes): an elbow from the crest to each row.
    for (u8 i = 0; i < kAbilityCount; ++i) {
        const f32 cy = rows_top + (static_cast<f32>(i) + 0.5f) * row_h;
        const f32 midx = (root_x + root_r + node_x) * 0.5f;
        draw.line(Vec2{root_x + root_r, root_y}, Vec2{midx, root_y}, 2.5f, branch);
        draw.line(Vec2{midx, root_y}, Vec2{midx, cy}, 2.5f, branch);
        draw.line(Vec2{midx, cy}, Vec2{node_x, cy}, 2.5f, branch);
    }

    // The crest: a rounded plate with the role's signature icon and name.
    draw.rect(Vec4{root_x - root_r, root_y - root_r, root_r * 2.0f, root_r * 2.0f},
              Vec4{0.16f, 0.12f, 0.08f, 0.98f}, Vec4{accent, 0.95f}, 2.5f, root_r * 0.35f);
    draw_ability_icon(draw, role_, 0, root_x, root_y - root_r * 0.18f, root_r * 0.5f,
                      Vec4{accent, 1.0f});
    draw.text(Vec2{root_x - draw.text_width(role_name(role_), 14.0f) * 0.5f, root_y + root_r * 0.42f},
              role_name(role_), 14.0f, th.text);

    // One node per ability: icon slot + (when equipped) its hotkey badge, then name, cooldown,
    // description and an equip status. Equipped nodes glow; the whole row is the click target.
    const f32 row_right = panel.x + panel.z - 24.0f;
    for (u8 i = 0; i < kAbilityCount; ++i) {
        const AbilityDef ab = ability_def(role_, i);
        const f32 ry = rows_top + static_cast<f32>(i) * row_h;
        const f32 cy = ry + row_h * 0.5f;
        const f32 bx = node_x;
        const f32 by = cy - icon_box * 0.5f;

        // Which hotbar slot (if any) this ability is bound to.
        int bound = -1;
        for (u8 s = 0; s < kAbilitySlots; ++s) {
            if (bar_[s] == static_cast<int>(i)) {
                bound = static_cast<int>(s);
            }
        }
        const bool equipped = bound >= 0;

        // The whole row is clickable to equip / unequip.
        skill_node_rects_[i] = ui::Rect{bx - 6.0f, ry + 3.0f, row_right - bx + 6.0f, row_h - 6.0f};
        if (equipped) { // a soft highlight band behind an equipped skill
            draw.rect(Vec4{bx - 6.0f, ry + 3.0f, row_right - bx + 6.0f, row_h - 6.0f},
                      Vec4{accent.r, accent.g, accent.b, 0.10f}, icon_box * 0.12f);
        }

        draw.rect(Vec4{bx, by, icon_box, icon_box}, Vec4{0.11f, 0.13f, 0.17f, 0.96f},
                  Vec4{accent, equipped ? 0.95f : 0.45f}, equipped ? 2.5f : 1.5f, icon_box * 0.16f);
        const Vec4 icol{equipped ? accent : accent * 0.6f, 1.0f};
        draw_ability_icon(draw, role_, i, bx + icon_box * 0.5f, by + icon_box * 0.5f,
                          icon_box * 0.26f, icol);
        if (equipped) { // a key badge showing which hotkey (1..4) casts it
            draw.rect(Vec4{bx + 4.0f, by + 4.0f, icon_box * 0.3f, icon_box * 0.3f},
                      Vec4{accent, 0.95f}, icon_box * 0.08f);
            draw.text(Vec2{bx + 4.0f + icon_box * 0.1f, by + 4.0f + icon_box * 0.06f},
                      std::format("{}", bound + 1), icon_box * 0.19f, Vec4{0.06f, 0.05f, 0.04f, 1.0f});
        }

        const f32 tx = bx + icon_box + 26.0f;
        const f32 name_sz = std::min(row_h * 0.24f, 22.0f);
        f32 ty = cy - name_sz - 8.0f;
        const Vec4 name_col{equipped ? th.text : Vec4{th.text.r, th.text.g, th.text.b, 0.75f}};
        const f32 nw = draw.text(Vec2{tx, ty}, ab.name, name_sz, name_col);
        draw.text(Vec2{tx + nw + 18.0f, ty + name_sz * 0.28f}, std::format("{:.0f}s CD", ab.cooldown),
                  name_sz * 0.62f, Vec4{accent, 0.85f});
        // Equip status on the right of the name line.
        const std::string status = equipped ? std::format("EQUIPPED - KEY {}", bound + 1)
                                             : "CLICK TO EQUIP";
        const f32 stsz = name_sz * 0.6f;
        draw.text(Vec2{row_right - draw.text_width(status, stsz), ty + name_sz * 0.3f}, status, stsz,
                  equipped ? Vec4{0.6f, 0.9f, 0.6f, 0.95f} : Vec4{accent, 0.7f});
        ty += name_sz + 8.0f;
        draw.text(Vec2{tx, ty}, ab.desc, std::min(row_h * 0.15f, 15.0f), th.text_muted);
    }

    draw.text(Vec2{hx, panel.y + panel.w - 30.0f}, "K / ESC  CLOSE", 16.0f, th.text_muted);
}

std::string ClientApp::town_name(const Vec3& c) {
    static const char* pre[] = {"Oak",  "Stone", "Black", "White", "Raven", "Wolf",
                                "Ash",  "Fern",  "Mill",  "Hart",  "Bram",  "Thorn",
                                "Wind", "Frost", "Elder", "Gold"};
    static const char* suf[] = {"ford",   "ton",  "wick", "field",  "bury", "dale",
                                "stead",  "hollow", "gate", "moor", "bridge", "haven",
                                "shire",  "mere", "crest", "wood"};
    u32 h = static_cast<u32>(static_cast<i32>(std::lround(c.x)) * 73856093) ^
            static_cast<u32>(static_cast<i32>(std::lround(c.z)) * 19349663);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    std::string name = pre[h % 16u];
    name += suf[(h >> 8) % 16u];
    return name;
}

} // namespace alryn::game
