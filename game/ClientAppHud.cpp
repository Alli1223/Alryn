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

    // Always-on corner minimap (hidden while the full M map is open).
    if (!map_open_) {
        draw_minimap(draw, feet, W, H);
    }

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
        // Intact-delivery bonus: keeping the wagon's health up earns up to +bonus pay on arrival, so
        // it's worth fighting the ambushers off rather than just outrunning them.
        const int ibonus = static_cast<int>(std::lround((intact_bonus_mult(wf) - 1.0f) * 100.0f));
        draw.text(Vec2{252.0f, 22.0f + ts * 2.6f - 1.0f}, std::format("INTACT +{}% PAY", ibonus),
                  ts * 0.62f, Vec4{glm::mix(Vec3{0.85f, 0.5f, 0.3f}, Vec3{0.6f, 0.86f, 0.5f}, wf), 1.0f});
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
        const bool wheel_off = wg.wheel_off != 0;
        const bool near_wheel = wheel_off && glm::length(wg.wheel_pos - feet) < kWheelPickupRange + 0.6f;
        std::string hint;
        if (wheel_off && near_wheel) {
            hint = "[E] pick up the wheel";
        } else if (wheel_off) {
            hint = "fetch the fallen wheel, carry it to the cart to refit";
        } else if (carrying) {
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
        // Wheel-off: a prominent centred alert + the re-attach progress bar while someone refits.
        if (wheel_off) {
            const char* warn = "WHEEL OFF! THE WAGON IS STRANDED";
            draw.text(Vec2{(W - draw.text_width(warn, ts * 0.95f)) * 0.5f, H * 0.28f}, warn,
                      ts * 0.95f, Vec4{0.96f, 0.55f, 0.28f, 1.0f});
            const f32 rf = static_cast<f32>(wg.repair) / 255.0f;
            if (rf > 0.01f) {
                const f32 bw = 260.0f;
                const f32 bx = (W - bw) * 0.5f, by = H * 0.28f + ts * 1.3f;
                draw.rect(Vec4{bx, by, bw, 14.0f}, Vec4{0.05f, 0.05f, 0.07f, 0.75f}, 3.0f);
                draw.rect(Vec4{bx, by, bw * rf, 14.0f}, Vec4{0.55f, 0.8f, 0.45f, 0.95f}, 3.0f);
                const std::string rt = std::format("RE-ATTACHING  {}%", static_cast<int>(rf * 100.0f));
                draw.text(Vec2{(W - draw.text_width(rt, ts * 0.6f)) * 0.5f, by + 18.0f}, rt,
                          ts * 0.6f, Vec4{0.8f, 0.88f, 0.78f, 1.0f});
            }
        } else if (short_load) {
            // Warn when crates have bounced out and aren't recovered (pay is dropping).
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

    draw.text(Vec2{24.0f, H - 52.0f}, "[M] MAP    [K] SKILLS    [U] GEAR", ts * 0.72f,
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

    // Mage combo casting: while holding Ctrl, show the queued elements + the spell they'll cast,
    // plus a hint. Element colours: fire/water/earth/nature.
    if (role_ == PlayerRole::Mage && (casting_ || combo_n_ > 0)) {
        static const Vec4 ecol[4] = {{1.0f, 0.5f, 0.2f, 1.0f},
                                     {0.4f, 0.65f, 1.0f, 1.0f},
                                     {0.6f, 0.45f, 0.3f, 1.0f},
                                     {0.45f, 0.85f, 0.45f, 1.0f}};
        const f32 sz = 34.0f, gap = 10.0f;
        const f32 total = kMaxCombo * sz + (kMaxCombo - 1) * gap;
        const f32 bx = (W - total) * 0.5f;
        const f32 by = H * 0.66f;
        draw.text(Vec2{(W - draw.text_width("WEAVING SPELL", ts * 0.72f)) * 0.5f, by - ts * 1.4f},
                  "WEAVING SPELL", ts * 0.72f, Vec4{0.8f, 0.7f, 1.0f, 1.0f});
        for (int i = 0; i < kMaxCombo; ++i) {
            const f32 sx = bx + static_cast<f32>(i) * (sz + gap);
            const bool filled = i < combo_n_;
            const Vec4 c = filled ? ecol[combo_[i] & 3] : Vec4{0.15f, 0.16f, 0.2f, 0.8f};
            draw.rect(Vec4{sx, by, sz, sz}, c, Vec4{0.7f, 0.6f, 1.0f, filled ? 0.95f : 0.4f}, 2.0f,
                      6.0f);
        }
        const auto spell = static_cast<SpellId>(resolve_combo());
        const char* sn = spell == SpellId::None ? "..." : spell_name(spell);
        draw.text(Vec2{(W - draw.text_width(sn, ts * 0.9f)) * 0.5f, by + sz + 8.0f}, sn, ts * 0.9f,
                  Vec4{0.95f, 0.88f, 1.0f, 1.0f});
        draw.text(Vec2{(W - draw.text_width("HOLD CTRL + 1-4 / WASD, RELEASE TO CAST", ts * 0.6f)) *
                           0.5f,
                       by + sz + 8.0f + ts},
                  "HOLD CTRL + 1-4 / WASD, RELEASE TO CAST", ts * 0.6f, Vec4{0.6f, 0.6f, 0.7f, 1.0f});
    } else if (role_ == PlayerRole::Mage) {
        // A standing hint so casting is discoverable (the bar keys do something for the Mage too).
        const char* hint = "TAP 1-4 TO CAST  -  HOLD CTRL FOR COMBOS (EARTH x3 = WALL, FIRE x2 = METEOR)";
        const f32 bar_top = H - glm::clamp(H * 0.092f, 56.0f, 84.0f) - 26.0f;
        draw.text(Vec2{(W - draw.text_width(hint, ts * 0.58f)) * 0.5f, bar_top - ts * 1.4f}, hint,
                  ts * 0.58f, Vec4{0.7f, 0.62f, 0.85f, 0.9f});
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
    // A per-contract modifier (derived from the wagon id) - hazardous / bulk / safe runs vary the
    // pay + ambush so the board isn't all the same; standard runs show nothing.
    if (const ContractModifier mod = contract_modifier(wg.id); mod != ContractModifier::Standard) {
        iy += ts * 1.25f;
        const Vec4 mcol = mod == ContractModifier::Safe   ? Vec4{0.6f, 0.85f, 0.95f, 1.0f}
                          : mod == ContractModifier::Bulk ? Vec4{0.82f, 0.74f, 0.96f, 1.0f}
                                                          : Vec4{0.98f, 0.55f, 0.45f, 1.0f};
        draw.text(Vec2{ix, iy}, modifier_name(mod), ts * 0.85f, mcol);
    }

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
        case PlayerRole::Mage: return Vec3{0.7f, 0.5f, 0.98f};     // arcane violet
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
        // The Mage shares ONE spell cooldown (mage_cd_) across its element slots; the slot's element
        // single-spell cooldown is the reference for the sweep. Other roles use the per-ability cd.
        const bool is_mage = role_ == PlayerRole::Mage;
        const f32 cd_now = is_mage ? mage_cd_ : ability_cd_[a];
        const f32 cd_ref =
            is_mage ? spell_cooldown(spell_for_combo(a == 0, a == 1, a == 2, a == 3)) : ab.cooldown;
        const f32 frac = cd_ref > 0.0f ? glm::clamp(cd_now / cd_ref, 0.0f, 1.0f) : 0.0f;
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
            const std::string secs = std::format("{:.0f}", std::ceil(cd_now));
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
            } else if (slot == 5) { // rally: a banner raised on a pole
                L(Vec2{-r * 0.45f, -r}, Vec2{-r * 0.45f, r});
                L(Vec2{-r * 0.45f, -r}, Vec2{r * 0.65f, -r * 0.7f});
                L(Vec2{r * 0.65f, -r * 0.7f}, Vec2{-r * 0.45f, -r * 0.4f});
            } else { // guardian leap: a leaping arc landing on a shield
                L(Vec2{-r, r}, Vec2{0.0f, -r});            // leap arc up
                L(Vec2{0.0f, -r}, Vec2{r, r * 0.2f});      // ... and down
                draw.rect(Vec4{cx + r * 0.4f, cy + r * 0.1f, r * 0.7f, r * 0.7f},
                          Vec4{0.0f, 0.0f, 0.0f, 0.0f}, c, th * 0.7f, r * 0.2f); // shield
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
            } else if (slot == 5) { // caltrops: a three-spoke jack of spikes
                for (int k = 0; k < 3; ++k) {
                    const f32 a = TwoPi * static_cast<f32>(k) / 6.0f;
                    const Vec2 d{std::cos(a), std::sin(a)};
                    L(d * -r, d * r);
                }
            } else { // war horn: a curved horn with sound waves
                L(Vec2{-r, -r * 0.4f}, Vec2{r * 0.2f, -r * 0.4f});
                L(Vec2{r * 0.2f, -r * 0.4f}, Vec2{r * 0.6f, r * 0.3f});
                L(Vec2{-r, -r * 0.4f}, Vec2{-r * 0.7f, r * 0.5f});
                for (int k = 0; k < 2; ++k) {
                    const f32 o = r * (0.55f + 0.3f * static_cast<f32>(k));
                    L(Vec2{o, -r * 0.7f}, Vec2{o + r * 0.25f, -r * 0.2f});
                    L(Vec2{o + r * 0.25f, -r * 0.2f}, Vec2{o, r * 0.3f});
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
            } else if (slot == 5) { // judgement: stacked downward strike chevrons
                for (int k = 0; k < 3; ++k) {
                    const f32 o = -r * 0.7f + static_cast<f32>(k) * r * 0.6f;
                    L(Vec2{-r * 0.7f, o}, Vec2{0.0f, o + r * 0.5f});
                    L(Vec2{0.0f, o + r * 0.5f}, Vec2{r * 0.7f, o});
                }
            } else { // empower: an upward power chevron + a spark
                L(Vec2{-r * 0.7f, r * 0.2f}, Vec2{0.0f, -r * 0.5f});
                L(Vec2{0.0f, -r * 0.5f}, Vec2{r * 0.7f, r * 0.2f});
                L(Vec2{-r * 0.7f, r * 0.7f}, Vec2{0.0f, 0.0f});
                L(Vec2{0.0f, 0.0f}, Vec2{r * 0.7f, r * 0.7f});
                L(Vec2{0.0f, -r}, Vec2{0.0f, -r * 0.55f}); // spark
            }
            break;
        case PlayerRole::Mage:
            if (slot == 0) { // fire: a flame
                L(Vec2{0.0f, r}, Vec2{-r * 0.4f, -r * 0.1f});
                L(Vec2{0.0f, r}, Vec2{r * 0.4f, -r * 0.1f});
                L(Vec2{-r * 0.4f, -r * 0.1f}, Vec2{0.0f, -r});
                L(Vec2{r * 0.4f, -r * 0.1f}, Vec2{0.0f, -r});
            } else if (slot == 1) { // water: a droplet
                L(Vec2{0.0f, -r}, Vec2{-r * 0.6f, r * 0.3f});
                L(Vec2{0.0f, -r}, Vec2{r * 0.6f, r * 0.3f});
                L(Vec2{-r * 0.6f, r * 0.3f}, Vec2{0.0f, r});
                L(Vec2{r * 0.6f, r * 0.3f}, Vec2{0.0f, r});
            } else if (slot == 2) { // earth: a stack of stones (diamond)
                L(Vec2{0.0f, -r}, Vec2{r, 0.0f});
                L(Vec2{r, 0.0f}, Vec2{0.0f, r});
                L(Vec2{0.0f, r}, Vec2{-r, 0.0f});
                L(Vec2{-r, 0.0f}, Vec2{0.0f, -r});
            } else if (slot == 3) { // nature: a leaf/sprig
                L(Vec2{0.0f, r}, Vec2{0.0f, -r * 0.6f});
                L(Vec2{0.0f, 0.0f}, Vec2{-r * 0.7f, -r * 0.5f});
                L(Vec2{0.0f, -r * 0.2f}, Vec2{r * 0.7f, -r * 0.7f});
            } else if (slot == 4) { // rock wall: a brick wall
                draw.rect(Vec4{cx - r, cy - r * 0.7f, r * 2.0f, r * 0.55f}, Vec4{0.0f}, c, th * 0.7f, 0.0f);
                draw.rect(Vec4{cx - r, cy + r * 0.15f, r * 2.0f, r * 0.55f}, Vec4{0.0f}, c, th * 0.7f, 0.0f);
                L(Vec2{0.0f, -r * 0.7f}, Vec2{0.0f, -r * 0.15f}); // brick joints
                L(Vec2{-r * 0.5f, r * 0.15f}, Vec2{-r * 0.5f, r * 0.7f});
                L(Vec2{r * 0.5f, r * 0.15f}, Vec2{r * 0.5f, r * 0.7f});
            } else if (slot == 5) { // meteor: a ball with motion streaks
                draw.rect(Vec4{cx - r * 0.45f, cy - r * 0.05f, r * 0.9f, r * 0.9f}, c, r * 0.45f);
                L(Vec2{-r * 0.2f, -r * 0.5f}, Vec2{-r * 0.8f, -r}); // streak
                L(Vec2{r * 0.2f, -r * 0.45f}, Vec2{-r * 0.3f, -r});
            } else { // rune of vigour: a circular rune with an inner spark (co-op buff)
                draw.rect(Vec4{cx - r * 0.85f, cy - r * 0.85f, r * 1.7f, r * 1.7f},
                          Vec4{0.0f, 0.0f, 0.0f, 0.0f}, c, th * 0.7f, r * 0.85f); // ring
                L(Vec2{0.0f, -r * 0.5f}, Vec2{0.0f, r * 0.5f});
                L(Vec2{-r * 0.45f, -r * 0.25f}, Vec2{r * 0.45f, -r * 0.25f});
                L(Vec2{-r * 0.45f, r * 0.25f}, Vec2{r * 0.45f, r * 0.25f});
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

// An always-on corner minimap: a small north-up window around the player showing nearby roads, the
// active haul's destination (clamped to the edge if off-window), enemy threats, and the player at
// centre with a facing tick. Cheap (lines + dots, no terrain raster) - the full M map stays the
// detailed pannable view.
void ClientApp::draw_minimap(ui::DrawList& draw, const Vec3& feet, f32 W, f32 H) {
    const f32 sz = glm::clamp(H * 0.2f, 132.0f, 200.0f);
    const Vec4 box{W - sz - 18.0f, 54.0f, sz, sz}; // top-right, just below the money counter
    const Vec2 c{box.x + sz * 0.5f, box.y + sz * 0.5f};
    constexpr f32 mm_radius = 120.0f; // world metres from centre to edge
    const f32 scale = (sz * 0.5f) / mm_radius;
    draw.rect(box, Vec4{0.04f, 0.05f, 0.07f, 0.62f}, Vec4{0.72f, 0.62f, 0.4f, 0.8f}, 2.0f, 8.0f);

    const Vec2 pxz{feet.x, feet.z};
    auto to_mm = [&](Vec2 wxz) { return c + Vec2{(wxz.x - pxz.x) * scale, (wxz.y - pxz.y) * scale}; };
    auto inside = [&](const Vec2& p) {
        return p.x >= box.x + 3.0f && p.x <= box.x + sz - 3.0f && p.y >= box.y + 3.0f &&
               p.y <= box.y + sz - 3.0f;
    };

    if (world_seed_ != 0) {
        for (const roads::Segment& s : roads::gather(pxz, mm_radius, world_seed_)) {
            const Vec2 a = to_mm(s.a), b = to_mm(s.b);
            if (inside(a) && inside(b)) {
                draw.line(a, b, 2.5f, Vec4{0.64f, 0.52f, 0.34f, 0.9f});
            }
        }
    }
    for (const net::EnemyState& e : snapshot_.enemies) { // threats, red
        const Vec2 p = to_mm(Vec2{e.position.x, e.position.z});
        if (inside(p)) {
            draw.rect(Vec4{p.x - 2.5f, p.y - 2.5f, 5.0f, 5.0f}, Vec4{0.92f, 0.27f, 0.2f, 1.0f}, 2.5f);
        }
    }
    if (snapshot_.contract_phase == static_cast<u8>(ContractPhase::Active) && !snapshot_.wagons.empty()) {
        const net::WagonState& wg = snapshot_.wagons.front(); // gold destination, edge-clamped
        Vec2 d = to_mm(Vec2{wg.dest.x, wg.dest.z});
        const f32 r = sz * 0.5f - 7.0f;
        if (const Vec2 off = d - c; glm::length(off) > r) {
            d = c + glm::normalize(off) * r;
        }
        draw.rect(Vec4{d.x - 4.0f, d.y - 4.0f, 8.0f, 8.0f}, Vec4{0.98f, 0.82f, 0.3f, 1.0f}, 4.0f);
    }
    // The player at centre + a heading tick.
    const Vec2 fwd{std::cos(face_yaw_), std::sin(face_yaw_)};
    draw.line(c, c + fwd * 11.0f, 2.5f, Vec4{0.9f, 0.95f, 1.0f, 1.0f});
    draw.rect(Vec4{c.x - 3.0f, c.y - 3.0f, 6.0f, 6.0f}, Vec4{0.95f, 0.97f, 1.0f, 1.0f}, 3.0f);
}

void ClientApp::rebuild_map_raster(const Vec4& panel, f32 ppm) {
    map_tiles_.clear();
    const Vec2 mc{panel.x + panel.z * 0.5f, panel.y + panel.w * 0.5f};
    const f32 top = panel.y + 38.0f, left = panel.x + 5.0f;
    const f32 right = panel.x + panel.z - 5.0f, bot = panel.y + panel.w - 5.0f;
    const f32 area_w = right - left, area_h = bot - top;
    const int cols = glm::clamp(static_cast<int>(area_w / 10.0f), 30, 110); // finer terrain detail
    const f32 cell = area_w / static_cast<f32>(cols);
    const int rows = std::max(1, static_cast<int>(std::ceil(area_h / cell)));
    const u32 seed = world_seed_;
    map_tiles_.reserve(static_cast<usize>(cols) * static_cast<usize>(rows));

    // A top-down terrain colour with relief hill-shading, so the map reads as the real landscape:
    // ocean/shallows by depth, beach, then the surface palette (grass/dirt/rock/snow), lit by a
    // soft directional shade computed from the local height gradient.
    auto terrain_color = [&](f32 wx, f32 wz) -> Vec4 {
        const f32 h = worldgen::height(wx, wz, seed);
        if (h < worldgen::water_level) {
            const f32 depth = glm::clamp((worldgen::water_level - h) / 8.0f, 0.0f, 1.0f);
            return Vec4{glm::mix(Vec3{0.24f, 0.46f, 0.56f}, Vec3{0.04f, 0.12f, 0.28f}, depth), 1.0f};
        }
        const f32 step = 4.0f;
        const f32 hx = worldgen::height(wx + step, wz, seed);
        const f32 hz = worldgen::height(wx, wz + step, seed);
        const Vec3 n = glm::normalize(Vec3{h - hx, step, h - hz});
        Vec3 c = worldgen::surface_color(Vec3{wx, h, wz}, Vec3{0.0f, 1.0f, 0.0f}, seed);
        // Lean each cell toward a clear canonical BIOME colour, so deserts / bogs / mountains /
        // snow read distinctly on the map (the in-world surface tints are deliberately subtle).
        auto biome_key = [](worldgen::Biome b) -> Vec3 {
            switch (b) {
                case worldgen::Biome::Desert: return {0.86f, 0.75f, 0.47f};
                case worldgen::Biome::Bog: return {0.27f, 0.31f, 0.20f};
                case worldgen::Biome::Mountains: return {0.55f, 0.54f, 0.57f};
                case worldgen::Biome::Snow: return {0.93f, 0.95f, 0.99f};
                case worldgen::Biome::Plains: return {0.56f, 0.63f, 0.34f};
                case worldgen::Biome::Beach: return {0.84f, 0.77f, 0.55f};
                default: return {0.28f, 0.46f, 0.22f}; // forest
            }
        };
        c = glm::mix(c, biome_key(worldgen::biome_at(wx, wz, seed)), 0.42f);
        if (h < worldgen::water_level + 0.7f) {
            c = glm::mix(c, Vec3{0.80f, 0.74f, 0.54f}, 0.55f); // beach band
        }
        const f32 shade =
            glm::clamp(glm::dot(n, glm::normalize(Vec3{0.5f, 0.85f, 0.35f})), 0.45f, 1.18f);
        return Vec4{glm::clamp(c * shade, Vec3{0.0f}, Vec3{1.0f}), 1.0f};
    };

    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i) {
            const f32 rx = left + static_cast<f32>(i) * cell;
            const f32 ry = top + static_cast<f32>(j) * cell;
            const f32 wx = map_center_.x + (rx + cell * 0.5f - mc.x) / ppm;
            const f32 wz = map_center_.y + (ry + cell * 0.5f - mc.y) / ppm;
            map_tiles_.emplace_back(Vec4{rx, ry, cell + 1.0f, cell + 1.0f}, terrain_color(wx, wz));
        }
    }
    map_raster_center_ = map_center_;
    map_raster_zoom_ = map_zoom_;
}

void ClientApp::draw_map() {
    if (renderer_ == nullptr) {
        return;
    }
    const VkExtent2D ext = renderer_->extent();
    const f32 W = static_cast<f32>(ext.width);
    const f32 H = static_cast<f32>(ext.height);
    ui::DrawList draw{*renderer_};

    draw.rect(Vec4{0.0f, 0.0f, W, H}, Vec4{0.03f, 0.04f, 0.06f, 0.86f});
    const f32 mg = std::min(W, H) * map::margin_frac;
    const Vec4 panel{mg, mg, W - 2.0f * mg, H - 2.0f * mg};
    draw.rect(panel, Vec4{0.07f, 0.08f, 0.10f, 0.98f}, 10.0f);

    const f32 inner = std::min(panel.z, panel.w) * 0.5f - 28.0f;
    const f32 view_world = map::view_world / map_zoom_; // zoom in -> smaller span -> more detail
    const f32 ppm = inner / view_world;
    map_ppm_ = ppm;
    const Vec2 mc{panel.x + panel.z * 0.5f, panel.y + panel.w * 0.5f};

    // Rebuild the cached terrain raster only when the view actually moved (so panning stays cheap).
    if (map_center_ != map_raster_center_ || map_zoom_ != map_raster_zoom_ ||
        ext.width != map_raster_ext_.x || ext.height != map_raster_ext_.y) {
        rebuild_map_raster(panel, ppm);
        map_raster_ext_ = UVec2{ext.width, ext.height};
    }
    for (const auto& [rect, col] : map_tiles_) {
        draw.rect(rect, col);
    }

    // Title strip + frame on top of the raster.
    draw.rect(Vec4{panel.x, panel.y, panel.z, 34.0f}, Vec4{0.06f, 0.07f, 0.10f, 0.96f}, 10.0f);
    draw.outline(panel, Vec4{0.40f, 0.36f, 0.28f, 1.0f}, 2.0f, 10.0f);
    draw.text(Vec2{panel.x + 22.0f, panel.y + 18.0f}, "WORLD MAP", 24.0f, Vec4{0.94f, 0.9f, 0.8f, 1.0f});

    auto to_screen = [&](f32 wx, f32 wz) {
        return Vec2{mc.x + (wx - map_center_.x) * ppm, mc.y + (wz - map_center_.y) * ppm};
    };
    auto in_panel = [&](const Vec2& p, f32 pad = 4.0f) {
        return p.x > panel.x + pad && p.x < panel.x + panel.z - pad &&
               p.y > panel.y + 36.0f && p.y < panel.y + panel.w - pad;
    };

    // Roads, coloured by the difficulty of the biome each stretch crosses (tan = easy lowland,
    // amber = moderate, red = a hard slog over mountains / through bog or desert).
    for (const roads::Segment& s : roads::gather(map_center_, view_world * 1.7f, world_seed_)) {
        const Vec2 a = to_screen(s.a.x, s.a.y), b = to_screen(s.b.x, s.b.y);
        if (!in_panel(a) && !in_panel(b)) {
            continue;
        }
        const f32 haz = roads::route_hazard(std::vector<Vec2>{s.a, s.b}, world_seed_);
        const Vec4 rc = haz < 0.25f  ? Vec4{0.66f, 0.55f, 0.36f, 0.95f}
                        : haz < 0.6f ? Vec4{0.86f, 0.62f, 0.26f, 0.95f}
                                     : Vec4{0.87f, 0.34f, 0.24f, 0.97f};
        draw.line(a, b, 2.5f, rc);
    }

    // The destination town (if a haul is active) gets highlighted.
    const net::WagonState* aw = active_wagon();
    const Vec2 dest = aw != nullptr ? Vec2{aw->dest.x, aw->dest.z} : Vec2{1e9f};

    // The active haul's full planned route, threaded through any intermediate towns, drawn bold gold
    // on top of the roads so you can read where the cart is headed.
    if (aw != nullptr) {
        const std::vector<Vec2> route = roads::route_through_towns(
            Vec2{aw->source.x, aw->source.z}, Vec2{aw->dest.x, aw->dest.z}, world_seed_);
        for (usize i = 1; i < route.size(); ++i) {
            const Vec2 a = to_screen(route[i - 1].x, route[i - 1].y);
            const Vec2 b = to_screen(route[i].x, route[i].y);
            if (in_panel(a) || in_panel(b)) {
                draw.line(a, b, 4.5f, Vec4{0.98f, 0.82f, 0.32f, 0.95f});
            }
        }
    }

    // Towns: a marker sized by the town's extent, its name above, the destination ringed gold.
    const int reach =
        static_cast<int>(view_world * 1.7f / worldgen::village_cell) + 1;
    const int ccx = static_cast<int>(std::floor(map_center_.x / worldgen::village_cell));
    const int ccz = static_cast<int>(std::floor(map_center_.y / worldgen::village_cell));
    for (int dz = -reach; dz <= reach; ++dz) {
        for (int dx = -reach; dx <= reach; ++dx) {
            const auto v = worldgen::village_at(ccx + dx, ccz + dz, world_seed_);
            if (!v) {
                continue;
            }
            const Vec2 c = to_screen(v->center.x, v->center.y);
            if (!in_panel(c, 0.0f)) {
                continue;
            }
            const bool is_dest = glm::length(v->center - dest) < 6.0f;
            const f32 r = glm::clamp(v->half * ppm, 4.0f, 50.0f);
            draw.rect(Vec4{c.x - r, c.y - r, 2.0f * r, 2.0f * r}, Vec4{0.80f, 0.70f, 0.48f, 0.95f},
                      Vec4{0.22f, 0.18f, 0.12f, 1.0f}, 2.0f, r * 0.35f);
            if (is_dest) {
                draw.outline(Vec4{c.x - r - 4.0f, c.y - r - 4.0f, 2.0f * r + 8.0f, 2.0f * r + 8.0f},
                             Vec4{0.98f, 0.82f, 0.32f, 1.0f}, 2.5f, r * 0.4f);
                // Danger pips (1..3) for the haul's difficulty, under the destination marker.
                const int dd = aw != nullptr ? glm::clamp<int>(aw->difficulty, 1, 3) : 1;
                for (int k = 0; k < dd; ++k) {
                    draw.rect(Vec4{c.x - static_cast<f32>(dd) * 4.5f + static_cast<f32>(k) * 9.0f,
                                   c.y + r + 4.0f, 6.0f, 6.0f},
                              Vec4{0.92f, 0.32f, 0.24f, 1.0f}, Vec4{0.1f, 0.05f, 0.05f, 1.0f}, 1.0f,
                              1.5f);
                }
            }
            const std::string name = town_name(Vec3{v->center.x, 0.0f, v->center.y});
            const f32 ns = glm::clamp(r * 0.9f, 12.0f, 18.0f);
            draw.text(Vec2{c.x - draw.text_width(name, ns) * 0.5f, c.y - r - ns - 3.0f}, name, ns,
                      is_dest ? Vec4{1.0f, 0.88f, 0.45f, 1.0f} : Vec4{0.96f, 0.92f, 0.82f, 1.0f});
        }
    }

    // Wagons: parked offers as small gold dots, the active cargo wagon prominently.
    if (have_snapshot_) {
        for (const net::WagonState& wg : snapshot_.wagons) {
            const Vec2 p = to_screen(wg.position.x, wg.position.z);
            if (!in_panel(p)) {
                continue;
            }
            const bool active = aw != nullptr && wg.id == aw->id;
            const f32 r = active ? 7.0f : 3.5f;
            draw.rect(Vec4{p.x - r, p.y - r, 2.0f * r, 2.0f * r},
                      Vec4{0.98f, 0.80f, 0.28f, 1.0f}, Vec4{0.25f, 0.18f, 0.05f, 1.0f}, 1.5f, 2.0f);
        }
    }

    // Players: each in the hair colour they chose; the local player is ringed white with a facing
    // tick. Drawn even when panned away, so you can always see where everyone is.
    if (have_snapshot_) {
        for (const net::PlayerState& pl : snapshot_.players) {
            const Vec2 p = to_screen(pl.position.x, pl.position.z);
            if (!in_panel(p)) {
                continue;
            }
            const Vec3 col = hair_color_of(pl.appearance.hair_color);
            const bool me = pl.id == my_id_;
            const f32 r = me ? 6.0f : 5.0f;
            if (me) {
                const Vec2 d{std::cos(face_yaw_), std::sin(face_yaw_)};
                draw.line(p, p + d * 15.0f, 3.0f, Vec4{1.0f, 1.0f, 1.0f, 0.95f});
            }
            draw.rect(Vec4{p.x - r, p.y - r, 2.0f * r, 2.0f * r}, Vec4{col, 1.0f},
                      Vec4{me ? Vec3{1.0f} : Vec3{0.05f}, 1.0f}, me ? 2.5f : 1.5f, r);
        }
    }

    // Legend (bottom-left): map symbols, the road-difficulty key, and the biome palette.
    constexpr int nleg = 14;
    const f32 lx = panel.x + 16.0f;
    f32 ly = panel.y + panel.w - 16.0f - static_cast<f32>(nleg) * 18.0f;
    draw.rect(Vec4{lx - 8.0f, ly - 10.0f, 186.0f, static_cast<f32>(nleg) * 18.0f + 14.0f},
              Vec4{0.05f, 0.06f, 0.09f, 0.88f}, Vec4{0.3f, 0.28f, 0.22f, 0.8f}, 1.5f, 6.0f);
    auto legend = [&](const Vec4& sw, const char* label, bool ring) {
        draw.rect(Vec4{lx, ly, 12.0f, 12.0f}, sw, ring ? Vec4{1.0f, 1.0f, 1.0f, 1.0f} : Vec4{0.0f},
                  ring ? 2.0f : 0.0f, 6.0f);
        draw.text(Vec2{lx + 20.0f, ly}, label, 13.0f, Vec4{0.86f, 0.86f, 0.9f, 1.0f});
        ly += 18.0f;
    };
    legend(Vec4{0.55f, 0.7f, 0.95f, 1.0f}, "YOU", true);
    legend(Vec4{0.74f, 0.24f, 0.13f, 1.0f}, "PLAYER", false);
    legend(Vec4{0.80f, 0.70f, 0.48f, 1.0f}, "TOWN", false);
    legend(Vec4{0.98f, 0.80f, 0.28f, 1.0f}, "WAGON / ROUTE", false);
    legend(Vec4{0.66f, 0.55f, 0.36f, 1.0f}, "ROAD - EASY", false);
    legend(Vec4{0.87f, 0.34f, 0.24f, 1.0f}, "ROAD - HARD", false);
    legend(Vec4{0.10f, 0.30f, 0.46f, 1.0f}, "WATER", false);
    legend(Vec4{0.28f, 0.46f, 0.22f, 1.0f}, "FOREST", false);
    legend(Vec4{0.56f, 0.63f, 0.34f, 1.0f}, "PLAINS", false);
    legend(Vec4{0.86f, 0.75f, 0.47f, 1.0f}, "DESERT", false);
    legend(Vec4{0.27f, 0.31f, 0.20f, 1.0f}, "BOG", false);
    legend(Vec4{0.55f, 0.54f, 0.57f, 1.0f}, "MOUNTAIN", false);
    legend(Vec4{0.93f, 0.95f, 0.99f, 1.0f}, "SNOW", false);
    legend(Vec4{0.92f, 0.32f, 0.24f, 1.0f}, "DANGER", false);

    draw.text(Vec2{panel.x + panel.z - 360.0f, panel.y + panel.w - 28.0f},
              "DRAG TO PAN   SCROLL ZOOM   M / ESC CLOSE", 15.0f, Vec4{0.72f, 0.74f, 0.8f, 1.0f});
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

// The GEAR / WARDROBE overlay (U): the party gold, the current kit + its stat bonus, a BUY-upgrade
// button (only in a town + affordable), recolour swatches, and a change-weapon button. Clicks are
// hit-tested in wardrobe_click() against the rects stashed here.
void ClientApp::draw_wardrobe() {
    if (renderer_ == nullptr) {
        return;
    }
    const VkExtent2D ext = renderer_->extent();
    const f32 W = static_cast<f32>(ext.width);
    const f32 H = static_cast<f32>(ext.height);
    ui::DrawList draw{*renderer_};
    const ui::Theme& th = ui::theme();
    const Vec3 accent = role_color(role_);

    draw.rect(Vec4{0.0f, 0.0f, W, H}, Vec4{0.03f, 0.02f, 0.015f, 0.86f});
    const f32 pw = std::min(W * 0.62f, 720.0f);
    const f32 ph = std::min(H * 0.82f, 600.0f);
    const Vec4 panel{(W - pw) * 0.5f, (H - ph) * 0.5f, pw, ph};
    draw.rect(panel, th.panel, Vec4{accent, 0.7f}, 2.5f, 12.0f);

    const f32 x = panel.x + 30.0f;
    f32 y = panel.y + 24.0f;
    draw.text(Vec2{x, y}, "WARDROBE", 34.0f, th.accent_hover);
    const std::string money = std::format("$ {}", snapshot_.money);
    draw.text(Vec2{panel.x + panel.z - draw.text_width(money, 24.0f) - 30.0f, y + 8.0f}, money, 24.0f,
              th.accent_hover);
    y += 58.0f;

    const net::PlayerState* me = local_player();
    const u8 owned = me != nullptr ? me->owned_tier : 0;
    const Vec3 feet = local_feet();
    const bool in_town =
        world_seed_ != 0 && worldgen::inside_village(feet.x, feet.z, world_seed_, 6.0f);

    // Current kit + its stat bonus.
    draw.text(Vec2{x, y},
              std::format("{}  -  {} KIT", role_name(role_), tier_name(static_cast<EquipmentTier>(owned))),
              22.0f, Vec4{accent, 1.0f});
    y += 30.0f;
    Equipment cur;
    cur.outfit_tier = owned;
    cur.weapon_tier = owned;
    const EquipBonus eb = equipment_bonus(cur);
    draw.text(Vec2{x, y},
              std::format("+{} HP    x{:.2f} DAMAGE    +{:.0f}% ARMOUR", static_cast<int>(eb.health_add),
                          eb.damage_mult, eb.mitigation_add * 100.0f),
              15.0f, th.text_muted);
    y += 36.0f;

    // Buy-upgrade button (server gates it on being in a town + affordability; this just requests it).
    wardrobe_buy_rect_ = ui::Rect{};
    if (static_cast<u8>(owned + 1) < kTierCount) {
        const auto next = static_cast<EquipmentTier>(owned + 1);
        const u32 price = tier_price(next);
        const bool affordable = snapshot_.money >= price;
        const bool can = in_town && affordable;
        const Vec4 btn{x, y, 340.0f, 46.0f};
        wardrobe_buy_rect_ = ui::Rect{btn.x, btn.y, btn.z, btn.w};
        draw.rect(btn, can ? Vec4{accent * 0.55f, 0.97f} : Vec4{0.2f, 0.18f, 0.16f, 0.9f},
                  Vec4{accent, can ? 0.95f : 0.3f}, 2.0f, 8.0f);
        draw.text(Vec2{btn.x + 16.0f, btn.y + 13.0f},
                  std::format("BUY {} KIT   -   $ {}", tier_name(next), price), 18.0f,
                  can ? th.text : th.text_muted);
        y += 54.0f;
        const char* hint = !in_town       ? "VISIT A TOWN SHOP TO BUY UPGRADES"
                           : !affordable  ? "NOT ENOUGH GOLD - DELIVER MORE CARGO"
                                          : "NICER LOOK + MORE HEALTH, DAMAGE & ARMOUR";
        draw.text(Vec2{x, y}, hint, 14.0f, th.text_muted);
        y += 34.0f;
    } else {
        draw.text(Vec2{x, y}, "FULLY UPGRADED  -  MASTER GEAR", 18.0f, th.accent_hover);
        y += 44.0f;
    }

    // Wagon-RIG upgrade (a money sink): reinforce the cart for more max health + ambush-damage resist.
    if (const u8 rl = snapshot_.rig_level; rl < kMaxRigLevel) {
        const u32 rprice = rig_price(static_cast<u8>(rl + 1));
        const bool rcan = in_town && snapshot_.money >= rprice;
        const Vec4 btn{x, y, 380.0f, 40.0f};
        wardrobe_rig_rect_ = ui::Rect{btn.x, btn.y, btn.z, btn.w};
        draw.rect(btn, rcan ? Vec4{accent * 0.55f, 0.97f} : Vec4{0.2f, 0.18f, 0.16f, 0.9f},
                  Vec4{accent, rcan ? 0.95f : 0.3f}, 2.0f, 8.0f);
        draw.text(Vec2{btn.x + 16.0f, btn.y + 11.0f},
                  std::format("REINFORCE WAGON  Lv {}   -   $ {}", rl + 1, rprice), 17.0f,
                  rcan ? th.text : th.text_muted);
        y += 50.0f;
    } else {
        wardrobe_rig_rect_ = ui::Rect{};
        draw.text(Vec2{x, y}, "WAGON FULLY REINFORCED", 17.0f, th.accent_hover);
        y += 34.0f;
    }

    // Recolour swatches (the player's chosen primary colour).
    draw.text(Vec2{x, y}, "OUTFIT COLOUR", 15.0f, th.text);
    y += 26.0f;
    const f32 sw = 46.0f;
    for (int i = 0; i < 8; ++i) {
        const Vec4 r{x + static_cast<f32>(i) * (sw + 8.0f), y, sw, sw};
        wardrobe_swatch_rects_[i] = ui::Rect{r.x, r.y, r.z, r.w};
        const Vec3 c = outfit_tint_of(static_cast<u8>(i));
        const bool sel = equip_loadout_.outfit_tint == static_cast<u8>(i);
        draw.rect(r, Vec4{c, 1.0f}, Vec4{sel ? Vec3{1.0f} : accent, sel ? 1.0f : 0.4f},
                  sel ? 3.0f : 1.5f, 6.0f);
    }
    y += sw + 28.0f;

    // Change weapon (cycles the role's options).
    const WeaponType wt = role_weapon(static_cast<u8>(role_), equip_loadout_.weapon_index);
    draw.text(Vec2{x, y}, std::format("WEAPON:   {}", weapon_name(wt)), 18.0f, Vec4{accent, 1.0f});
    wardrobe_weapon_rect_ = ui::Rect{};
    if (role_weapon_count(static_cast<u8>(role_)) > 1) {
        const Vec4 btn{x + 280.0f, y - 8.0f, 140.0f, 36.0f};
        wardrobe_weapon_rect_ = ui::Rect{btn.x, btn.y, btn.z, btn.w};
        draw.rect(btn, Vec4{accent * 0.55f, 0.97f}, Vec4{accent, 0.95f}, 2.0f, 8.0f);
        draw.text(Vec2{btn.x + 22.0f, btn.y + 9.0f}, "CHANGE", 16.0f, th.text);
    }

    draw.text(Vec2{x, panel.y + panel.w - 34.0f}, "PRESS U OR ESC TO CLOSE", 14.0f, th.text_muted);
}

void ClientApp::wardrobe_click(const Vec2& p) {
    if (in_rect(p, wardrobe_buy_rect_)) {
        const net::PlayerState* me = local_player();
        const u8 owned = me != nullptr ? me->owned_tier : 0;
        if (static_cast<u8>(owned + 1) < kTierCount) {
            pending_buy_ = static_cast<u8>(owned + 1); // request the next tier (server gates it)
        }
        return;
    }
    if (in_rect(p, wardrobe_rig_rect_)) {
        if (const u8 rl = snapshot_.rig_level; rl < kMaxRigLevel) {
            pending_buy_rig_ = static_cast<u8>(rl + 1); // request the next rig level (server gates it)
        }
        return;
    }
    if (in_rect(p, wardrobe_weapon_rect_)) {
        const u8 n = role_weapon_count(static_cast<u8>(role_));
        if (n > 1) {
            equip_loadout_.weapon_index = static_cast<u8>((equip_loadout_.weapon_index + 1) % n);
        }
        return;
    }
    for (int i = 0; i < 8; ++i) {
        if (in_rect(p, wardrobe_swatch_rects_[i])) {
            equip_loadout_.outfit_tint = static_cast<u8>(i);
            return;
        }
    }
}

} // namespace alryn::game
