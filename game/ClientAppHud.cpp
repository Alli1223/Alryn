// ClientApp - in-game HUD, contract panel, ability bar, world map and health bars.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"
#include "HudStyle.h"

#include <functional>

namespace alryn::game {

namespace {
// The world map's raster fills its framed board inset by this much on every side.
constexpr f32 kMapInset = 8.0f;
} // namespace

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
        const f32 bw = glm::clamp(300.0f / clip.w, 18.0f, 52.0f);
        const f32 bh = 5.5f;
        frac = glm::clamp(frac, 0.0f, 1.0f);
        // A dark-rimmed pill with a glossy gradient fill (lighter on top), like the HUD gauges.
        ui::DrawList d{*renderer_};
        d.rect(Vec4{sx - bw * 0.5f - 1.5f, sy - 1.5f, bw + 3.0f, bh + 3.0f},
               Vec4{0.05f, 0.03f, 0.02f, 0.8f}, 3.0f);
        if (frac > 0.0f) {
            d.gradient(Vec4{sx - bw * 0.5f, sy, std::max(bw * frac, 3.0f), bh},
                       Vec4{glm::mix(col, Vec3{1.0f}, 0.3f), 1.0f}, Vec4{col * 0.75f, 1.0f}, 2.0f);
        }
    };
    for (const net::EnemyState& en : snapshot_.enemies) {
        const f32 hgt = en.kind == 2 ? 3.0f : en.kind == kEnemyAlpha ? 1.9f : en.kind == kEnemyWolf ? 1.5f : 2.2f;
        bar(en.position + Vec3{0.0f, hgt, 0.0f}, static_cast<f32>(en.health) / 255.0f,
            Vec3{0.92f, 0.26f, 0.2f});
    }
    for (const net::VillagerState& vl : snapshot_.villagers) {
        if ((vl.kind == 0 || vl.kind >= 5) && vl.health >= 250) {
            continue; // hide bars over healthy villagers + travellers (only show the hurt)
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

// The floating combat labels ("SHATTER!" / "EMPOWERED!" / "CANNONBALL!"): each pops in over the
// spot it happened, drifts upward and fades out - centred, so the eye reads it without hunting.
void ClientApp::draw_combat_text(ui::DrawList& draw, f32 W, f32 H) {
    for (const FloatText& ft : float_texts_) {
        const f32 t = ft.age / ft.life; // 0 -> 1 over its life
        Vec2 sp;
        if (!world_to_screen(ft.world + Vec3{0.0f, t * 1.1f, 0.0f}, W, H, sp)) {
            continue;
        }
        const f32 pop = t < 0.12f ? t / 0.12f : 1.0f;             // quick scale-in
        const f32 alpha = t > 0.6f ? 1.0f - (t - 0.6f) / 0.4f : 1.0f; // hold, then fade
        const f32 sz = ft.size * (0.7f + 0.3f * pop);
        // Punchy outlined capitals with a lighter top, so a call-out pops off the scene.
        ui::TextStyle st = hud::style(sz, Vec4{glm::mix(Vec3{ft.color}, Vec3{1.0f}, 0.35f), ft.color.a * alpha},
                                      ui::TextAlign::Center);
        st.color_bottom = Vec4{Vec3{ft.color} * 0.85f, ft.color.a * alpha};
        st.outline_width = std::max(1.5f, sz * 0.12f);
        draw.text(sp, ft.text, st);
    }
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

    // Shared party money, top-right: a gold coin + the purse in a dark-wood pill. A fresh gain (a
    // bandit's spilled purse, a delivery) flashes the pill gold and floats a "+n" up beside it.
    const f32 margin = 16.0f;
    const f32 pill_h = ts * 1.9f;
    {
        const std::string money = std::format("{}", snapshot_.money);
        const f32 ms = ts * 1.0f;
        const f32 pill_w = hud::width(money, ms) + pill_h + ts * 0.9f;
        const Vec4 pill{W - margin - pill_w, margin, pill_w, pill_h};
        if (money_pulse_ > 0.0f) {
            draw.shadow(pill, pill_h * 0.5f, 14.0f, hud::alpha(hud::kGold, 0.45f * money_pulse_));
        }
        hud::plaque(draw, pill, 0.86f, pill_h * 0.5f);
        hud::coin(draw, Vec2{pill.x + pill_h * 0.5f + 2.0f, pill.y + pill_h * 0.5f}, pill_h * 0.33f);
        hud::text(draw, Vec2{pill.x + pill_h + ts * 0.2f, pill.y + (pill_h - ms) * 0.5f}, money, ms,
                  hud::kGold);
        if (money_pulse_ > 0.0f && money_gain_ > 0) {
            const f32 gs = ts * 0.8f;
            hud::text(draw,
                      Vec2{pill.x - ts * 0.5f, pill.y + (pill_h - gs) * 0.5f - (1.0f - money_pulse_) * 14.0f},
                      std::format("+{}", money_gain_), gs, hud::alpha(hud::kGold, money_pulse_),
                      ui::TextAlign::Right);
        }
        // Clean-delivery streak (perfect full-cargo runs) + its stacking pay bonus, a chip just
        // left of the purse.
        if (snapshot_.delivery_streak > 0) {
            const u32 s = snapshot_.delivery_streak;
            const int pct = static_cast<int>(std::lround((streak_mult(s) - 1.0f) * 100.0f));
            const std::string str = std::format("STREAK x{}  +{}%", s, pct);
            const f32 cs = ts * 0.62f;
            const f32 cw = hud::width(str, cs) + cs * 1.2f;
            hud::chip(draw, Vec2{pill.x - cw - (money_pulse_ > 0.0f ? ts * 3.2f : ts * 0.5f),
                                 pill.y + (pill_h - cs * 1.7f) * 0.5f},
                      str, cs, hud::kGood);
        }
    }
    draw_combat_text(draw, W, H); // world-anchored "SHATTER!" / "EMPOWERED!" / "CANNONBALL!" labels
    draw_charge_meter(draw, W, H); // the heavy attack's wind-up gauge under the hero
    draw_nameplates(draw, W, H);  // teammates' names in their colours (+ edge pointers when off-screen)
    draw_party_frames(draw, W, H, ts);

    // Always-on corner minimap (hidden while the full M map is open).
    if (!map_open_) {
        draw_minimap(draw, feet, W, H);
    }

    // Top-left objective card: content rows are laid out first, then drawn over a plaque sized to
    // fit them (so the card always hugs its text, however many status lines are showing).
    struct Row {
        f32 h = 0.0f;
        f32 w = 0.0f;
        std::function<void(f32 x, f32 y)> paint;
    };
    std::vector<Row> rows;
    auto add_text = [&](std::string s, f32 size, Vec4 col, f32 gap_after = 0.45f) {
        const f32 w = hud::width(s, size);
        rows.push_back({size * (1.0f + gap_after), w, [&draw, s = std::move(s), size, col](f32 x, f32 y) {
                            hud::text(draw, Vec2{x, y}, s, size, col);
                        }});
    };
    auto add_rich = [&](std::string s, f32 size, Vec4 col) {
        const f32 w = hud::rich_width(s, size);
        rows.push_back({size * 1.75f, w, [&draw, s = std::move(s), size, col](f32 x, f32 y) {
                            hud::rich(draw, Vec2{x, y + size * 0.15f}, s, size, col);
                        }});
    };
    auto add_heading = [&](std::string s, f32 size) {
        const f32 w = hud::heading_width(s, size);
        rows.push_back({size * 1.55f, w, [&draw, s = std::move(s), size](f32 x, f32 y) {
                            hud::heading(draw, Vec2{x, y}, s, size);
                        }});
    };
    auto paint_card = [&](f32 min_w) {
        if (rows.empty()) {
            return;
        }
        const f32 pad = ts * 0.8f;
        f32 cw = min_w, ch = 0.0f;
        for (const Row& r : rows) {
            cw = std::max(cw, r.w);
            ch += r.h;
        }
        hud::plaque(draw, Vec4{margin, margin, cw + pad * 2.0f, ch + pad * 1.6f});
        objective_bottom_ = margin + ch + pad * 1.6f;
        f32 y = margin + pad * 0.9f;
        for (const Row& r : rows) {
            r.paint(margin + pad, y);
            y += r.h;
        }
    };

    // A side quest under way gets its own lines on the card: what it is, what to do, how far along,
    // and how far off the site lies.
    auto add_quest = [&]() {
        const net::QuestState* q = active_quest();
        if (q == nullptr) {
            return;
        }
        const auto kind = static_cast<QuestKind>(q->kind);
        rows.push_back({ts * 0.55f, 0.0f, [&draw, ts](f32 x, f32 y) {
                            draw.line(Vec2{x, y + ts * 0.22f}, Vec2{x + ts * 9.0f, y + ts * 0.22f}, 1.0f,
                                      hud::alpha(ui::theme().accent, 0.45f));
                        }});
        add_text(std::format("SIDE QUEST  -  {}", quest_title(kind)), ts * 0.56f, Vec4{0.72f, 0.9f, 1.0f, 1.0f}, 0.3f);
        const f32 dist = glm::length(Vec2{q->site.x - feet.x, q->site.z - feet.z});
        std::string line = quest_objective(kind);
        if (q->goal > 1 && q->phase == static_cast<u8>(QuestPhase::Active)) {
            line += std::format("   {} / {}", q->progress, q->goal);
        }
        add_rich(line, ts * 0.6f, ui::theme().text);
        if (dist > 18.0f) {
            add_text(std::format("~{} M AWAY", static_cast<int>(dist)), ts * 0.5f, hud::alpha(ui::theme().text_muted, 0.95f),
                     0.25f);
        }
    };

    // A roadside errand under way: its lines too.
    auto add_errand = [&]() {
        const net::ErrandState* e = current_errand();
        if (e == nullptr || e->phase != static_cast<u8>(QuestPhase::Active)) {
            return;
        }
        const auto kind = static_cast<ErrandKind>(e->kind);
        add_text(std::format("ERRAND  -  {}", errand_title(kind)), ts * 0.56f, Vec4{0.95f, 0.88f, 0.62f, 1.0f}, 0.3f);
        std::string line = errand_objective(kind);
        if (e->goal > 1) {
            line += std::format("   {} / {}", e->progress, e->goal);
        }
        add_rich(line, ts * 0.6f, ui::theme().text);
    };

    // The hero's JOURNEY - the linear spine of goals - closes out the objective card: the current
    // step, what to do, and progress toward a counted goal.
    auto add_journey = [&]() {
        add_quest();
        add_errand();
        const u8 step = std::min<u8>(live_progress_.journey, kJourneySteps);
        const JourneyStep js = journey_step(step);
        rows.push_back({ts * 0.55f, 0.0f, [&draw, ts](f32 x, f32 y) {
                            draw.line(Vec2{x, y + ts * 0.22f}, Vec2{x + ts * 9.0f, y + ts * 0.22f}, 1.0f,
                                      hud::alpha(ui::theme().accent, 0.45f));
                        }});
        const std::string head = step >= kJourneySteps ? std::string{"JOURNEY COMPLETE"}
                                                       : std::format("JOURNEY {} / {}", step + 1, kJourneySteps);
        add_text(head, ts * 0.5f, hud::alpha(ui::theme().text_muted, 0.95f), 0.3f);
        std::string title = js.title;
        if (js.target > 0) {
            HeroRecord rec;
            rec.kills = live_progress_.kills;
            title += std::format("   {} / {}", journey_count(step, rec), js.target);
        }
        add_text(title, ts * 0.72f, hud::kGold, 0.3f);
        add_rich(js.hint, ts * 0.52f, ui::theme().text);
    };

    if (phase == static_cast<u8>(ContractPhase::Offer)) {
        add_heading("FIND A CONTRACT", ts * 0.95f);
        add_text("WALK UP TO A WAGON TO VIEW ITS CONTRACT", ts * 0.68f, ui::theme().text, 0.2f);
        add_journey();
        paint_card(0.0f);
        // A small floating tag over every offered wagon so you can see where they are and
        // pick which to walk to (gold reward; the one you're next to gets the full panel).
        for (const net::WagonState& wg : snapshot_.wagons) {
            if (wg.id == selected_wagon_ || wg.id == near_wagon_) {
                continue; // the focused wagon gets the full panel instead
            }
            Vec2 sp;
            if (world_to_screen(wg.position + Vec3{0.0f, 2.6f, 0.0f}, W, H, sp)) {
                const std::string tag = std::format("{}", wg.reward);
                const f32 gs = ts * 0.72f;
                const f32 tw = hud::width(tag, gs) + gs * 2.2f;
                const Vec4 r{sp.x - tw * 0.5f, sp.y - gs * 0.5f, tw, gs * 1.8f};
                hud::plaque(draw, r, 0.8f, r.w * 0.5f);
                hud::coin(draw, Vec2{r.x + gs * 0.95f, r.y + r.w * 0.5f}, gs * 0.5f);
                hud::text(draw, Vec2{r.x + gs * 1.75f, r.y + (r.w - gs) * 0.5f}, tag, gs, hud::kGold);
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
        // A tall-walled (enclosed) bed carries the noble - same rule generate_offers uses to
        // assign Passengers cargo, so the client derives VIP without an extra wire field.
        const bool vip = vt.bed().wall > 2.0f;
        std::string title = vt.name();
        for (char& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        add_heading(vip ? std::string{"ESCORT THE NOBLE"} : std::format("DELIVER THE {}", title),
                    ts * 0.95f);
        add_text(std::format("{} GOLD   -   ~{} M TO GO{}", wg.reward, static_cast<int>(dist),
                             manual ? "   -   HAULING BY HAND" : ""),
                 ts * 0.66f, hud::kGold, 0.55f);
        // Cargo load (pay scales with the share delivered); a VIP haul has no crates to spill.
        const bool short_load = !vip && wg.goods_aboard < wg.goods_total;
        if (vip) {
            add_text("VIP - RAIDERS TARGET THE CARRIAGE", ts * 0.62f, hud::kWarn);
        } else {
            add_text(std::format("GOODS ABOARD  {} / {}", wg.goods_aboard, wg.goods_total), ts * 0.62f,
                     short_load ? hud::kWarn : ui::theme().text);
        }
        // Wagon health gauge (green -> red), with a pale trail of recent damage draining away.
        const f32 wf = static_cast<f32>(wg.health) / 255.0f;
        hud_wagon_ghost_ = std::max(wf, hud_wagon_ghost_ - frame_dt_ * 0.35f);
        {
            const f32 gw = std::max(ts * 13.0f, 220.0f);
            const Vec3 wc = glm::mix(Vec3{0.86f, 0.28f, 0.2f}, Vec3{0.45f, 0.78f, 0.36f}, wf);
            rows.push_back({ts * 1.35f, gw, [&draw, wf, wc, gw, ts, ghost = hud_wagon_ghost_](f32 x, f32 y) {
                                hud::bar(draw, Vec4{x, y, gw, ts * 0.72f}, wf,
                                         Vec4{glm::mix(wc, Vec3{1.0f}, 0.25f), 1.0f}, Vec4{wc * 0.7f, 1.0f},
                                         4, ghost);
                                hud::text(draw, Vec2{x + gw * 0.5f, y + (ts * 0.72f - ts * 0.5f) * 0.5f},
                                          std::format("WAGON  {}%", static_cast<int>(std::lround(wf * 100.0f))),
                                          ts * 0.5f, Vec4{1.0f}, ui::TextAlign::Center);
                            }});
        }
        // LAST STAND: a near-wrecked wagon rallies the defenders (ramping their damage) - flag it.
        if (wf > 0.0f && wf < kLastStandThreshold) {
            const int lsb = static_cast<int>(std::lround((last_stand_mult(wf) - 1.0f) * 100.0f));
            const f32 pulse = 0.55f + 0.45f * std::sin(elapsed_ * 9.0f);
            add_text(std::format("LAST STAND  +{}% DAMAGE", lsb), ts * 0.72f,
                     Vec4{1.0f, 0.3f + 0.25f * pulse, 0.2f, 1.0f});
        } else if (wf > 0.0f && wf < 0.999f && snapshot_.enemies.empty()) {
            // Damaged + no raiders left: prompt the field-repair (stay near the cart to mend it).
            add_text("TEND THE CART TO REPAIR IT", ts * 0.66f, hud::kGood);
        }
        // The pay modifiers as a row of chips: intact-delivery bonus (worth fighting the ambushers
        // off rather than outrunning them), rush bonus (delivering fast pays extra, decaying over the
        // route - an approximate client estimate, route ~1.3x the straight line; the server's payout
        // is authoritative) and the kill bounty for raiders felled this haul.
        {
            const int ibonus = static_cast<int>(std::lround((intact_bonus_mult(wf) - 1.0f) * 100.0f));
            const f32 sld = glm::length(Vec2{wg.dest.x - wg.source.x, wg.dest.z - wg.source.z}) * 1.3f;
            const int rbonus = static_cast<int>(
                std::lround((rush_bonus_mult(haul_elapsed_, rush_expected_time(sld)) - 1.0f) * 100.0f));
            std::vector<std::pair<std::string, Vec4>> chips;
            chips.emplace_back(std::format("INTACT +{}%", ibonus),
                               Vec4{glm::mix(Vec3{0.92f, 0.52f, 0.3f}, Vec3{0.6f, 0.88f, 0.5f}, wf), 1.0f});
            if (rbonus > 0) {
                chips.emplace_back(std::format("RUSH +{}%", rbonus), hud::kGold);
            }
            if (snapshot_.contract_kills > 0) {
                chips.emplace_back(std::format("BOUNTY {}  ({} DOWN)", kill_bounty(snapshot_.contract_kills),
                                               snapshot_.contract_kills),
                                   Vec4{1.0f, 0.6f, 0.52f, 1.0f});
            }
            const f32 cs = ts * 0.52f;
            f32 cw = 0.0f;
            for (const auto& [s, c] : chips) {
                cw += hud::width(s, cs) + cs * 1.2f + cs * 0.5f;
            }
            rows.push_back({cs * 2.4f, cw, [&draw, chips, cs](f32 x, f32 y) {
                                for (const auto& [s, c] : chips) {
                                    x += hud::chip(draw, Vec2{x, y}, s, cs, c) + cs * 0.5f;
                                }
                            }});
        }
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
        for (char& c : hint) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        add_rich(hint, ts * 0.62f, ui::theme().text);
        add_journey();
        paint_card(0.0f);
        // Wheel-off: a prominent centred alert + the re-attach progress bar while someone refits.
        if (wheel_off) {
            hud::banner(draw, W * 0.5f, H * 0.24f, "WHEEL OFF - THE WAGON IS STRANDED", ts * 0.95f, hud::kWarn);
            const f32 rf = static_cast<f32>(wg.repair) / 255.0f;
            if (rf > 0.01f) {
                const f32 bw = std::max(260.0f, ts * 14.0f);
                const f32 bx = (W - bw) * 0.5f, by = H * 0.24f + ts * 2.2f;
                hud::bar(draw, Vec4{bx, by, bw, ts * 0.7f}, rf, Vec4{0.7f, 0.92f, 0.55f, 1.0f},
                         Vec4{0.35f, 0.62f, 0.28f, 1.0f});
                hud::text(draw, Vec2{W * 0.5f, by + ts * 1.05f},
                          std::format("RE-ATTACHING  {}%", static_cast<int>(rf * 100.0f)), ts * 0.6f,
                          ui::theme().text, ui::TextAlign::Center);
            }
        } else if (short_load) {
            // Warn when crates have bounced out and aren't recovered (pay is dropping).
            hud::banner(draw, W * 0.5f, H * 0.24f, "CARGO SPILLING - RECOVER THE CRATES", ts * 0.85f,
                        hud::kWarn);
        }
        draw_dest_arrow(draw, feet, Vec3{wg.dest.x, feet.y, wg.dest.z}, W);
    } else {
        add_journey();
        paint_card(0.0f);
    }

    draw_quest_hud(draw, W, H, ts); // the notice board's panel, the quest's waypoint + its triumph banner
    draw_errand_hud(draw, W, H, ts); // the roadside traveller: their "!", what they ask, the way + the thanks

    // Settle banner: the haul's outcome, big and centred, in green or red.
    if (snapshot_.contract_outcome != 0) {
        const bool ok = snapshot_.contract_outcome == 1;
        const f32 bs = glm::clamp(H * 0.055f, 26.0f, 64.0f);
        hud::banner(draw, W * 0.5f, H * 0.3f, ok ? "WAGON DELIVERED" : "WAGON LOST", bs,
                    ok ? hud::kGood : hud::kBad);
    }

    // Bottom-left: the unit frame (role crest + name + health gauge) on a plaque, with the control
    // hints stacked above it as key-cap rows.
    const f32 bw = std::min(ts * 15.0f, W * 0.26f);
    const f32 bh = ts * 0.85f;
    const f32 frame_pad = ts * 0.65f;
    const f32 crest = ts * 2.5f;
    const Vec4 frame{margin, H - margin - (crest + frame_pad * 2.0f),
                     crest + bw + frame_pad * 3.0f, crest + frame_pad * 2.0f};
    const f32 x = margin + 2.0f;
    const f32 controls_y = frame.y - ts * 1.25f;  // [M] / [K] / [U] row, just above the frame
    // CONTEXTUAL co-op prompts: when a teammate is actually in reach the hint NAMES them and
    // brightens, so the combos advertise themselves at the moment they're possible.
    const net::PlayerState* near_ally = nullptr;
    f32 near_d = 1e9f;
    for (const net::PlayerState& p : snapshot_.players) {
        if (p.id == my_id_) {
            continue;
        }
        const f32 d = glm::length(p.position - feet);
        if (d < near_d) {
            near_d = d;
            near_ally = &p;
        }
    }
    const auto ally_role = [&] {
        return role_name(static_cast<PlayerRole>(near_ally->role % kRoleCount));
    };
    const bool can_toss = near_ally != nullptr && near_d <= kTossGrabRange;
    std::string combo_hint = can_toss ? std::format("[G] TOSS THE {}", ally_role())
                                      : "[G] TOSS ALLY (STAND CLOSE)";
    bool can_combo = can_toss;
    if (role_ == PlayerRole::Cleric) {
        // The Power Conduit hint only shows for a Cleric (V is a no-op for other roles).
        const bool can_beam = near_ally != nullptr && near_d <= kConduitRange;
        combo_hint += can_beam ? std::format("   [V] CONDUIT > {}", ally_role()) : "   [V] CONDUIT";
        can_combo = can_combo || can_beam;
    }
    const f32 hs = ts * 0.64f;
    hud::rich(draw, Vec2{x, controls_y - hs * 2.0f}, combo_hint, hs,
              can_combo ? hud::kGold : hud::alpha(ui::theme().text, 0.85f));
    hud::rich(draw, Vec2{x, controls_y - hs * 4.0f}, "HOLD [LMB] HEAVY ATTACK   [Q] DIG", hs,
              hud::alpha(ui::theme().text, 0.85f));
    {
        // An unspent skill point makes the [K] hint glow, so levelling up leads straight to the tree.
        const i32 pts = points_available(role_, hero_level(), known_mask(), live_progress_.talents);
        const f32 cw = hud::rich(draw, Vec2{x, controls_y}, "[M] MAP   [K] SKILLS   [U] GEAR   [J] JOURNEY   [L] LANTERN",
                                 hs, ui::theme().text);
        if (pts > 0) {
            const f32 pulse = 0.82f + 0.18f * std::sin(elapsed_ * 5.0f);
            hud::chip(draw, Vec2{x + cw + hs * 0.6f, controls_y - hs * 0.3f},
                      std::format("{} SKILL POINT{}", pts, pts == 1 ? "" : "S"), hs * 0.8f,
                      Vec4{1.0f, 0.85f, 0.45f, pulse});
        }
    }

    hud::plaque(draw, frame, 0.86f);
    // The crest: the role's signature icon on a round gold-rimmed boss, in the role's colour.
    const Vec3 rc = role_color(role_);
    const Vec2 cc{frame.x + frame_pad + crest * 0.5f, frame.y + frame.w * 0.5f};
    hud::medallion(draw, cc, crest * 0.5f - 4.0f, Vec4{0.06f, 0.04f, 0.03f, 1.0f});
    draw_ability_icon(draw, role_, 0, cc.x, cc.y, crest * 0.2f, Vec4{rc, 1.0f});
    // The hero's name (in their identity colour) + level over the health gauge, which trails a pale
    // ghost of recent damage as it drains; a thin XP gauge runs along the frame's foot.
    const f32 gx = frame.x + frame_pad * 2.0f + crest;
    const f32 name_s = ts * 0.74f;
    const Vec3 pc = player_color(live_color_);
    const f32 nw = hud::text(draw, Vec2{gx, frame.y + frame_pad * 0.9f}, hero_.name, name_s,
                             Vec4{glm::mix(pc, Vec3{1.0f}, 0.4f), 1.0f}, ui::TextAlign::Left, ui::FontFace::Display);
    hud::chip(draw, Vec2{gx + nw + ts * 0.4f, frame.y + frame_pad * 0.9f - ts * 0.05f},
              std::format("LV {}  {}", hero_level(), role_name(role_)), ts * 0.46f, hud::kGold);
    {
        const f32 xf = level_progress(live_progress_.xp);
        const Vec4 xb{frame.x + 8.0f, frame.y + frame.w - 6.0f, frame.z - 16.0f, 3.5f};
        draw.rect(xb, Vec4{0.02f, 0.015f, 0.01f, 0.85f}, 1.75f);
        if (xf > 0.0f) {
            draw.gradient(Vec4{xb.x, xb.y, xb.z * xf, xb.w}, hud::kGold, hud::shade(hud::kGold, 0.7f), 1.75f);
        }
    }
    hud_hp_ghost_ = std::max(hp, hud_hp_ghost_ - frame_dt_ * 0.45f);
    const Vec4 gauge{gx, frame.y + frame.w - frame_pad - bh, bw, bh};
    const Vec3 col = glm::mix(Vec3{0.88f, 0.22f, 0.18f}, Vec3{0.38f, 0.8f, 0.32f}, hp);
    hud::bar(draw, gauge, hp, Vec4{glm::mix(col, Vec3{1.0f}, 0.25f), 1.0f}, Vec4{col * 0.68f, 1.0f}, 10,
             hud_hp_ghost_);
    hud::text(draw, Vec2{gauge.x + gauge.z * 0.5f, gauge.y + (bh - ts * 0.55f) * 0.5f},
              std::format("{} %", static_cast<int>(std::lround(std::max(hp, 0.0f) * 100.0f))), ts * 0.55f,
              Vec4{1.0f}, ui::TextAlign::Center);

    // Cleric heal-channel charge bar (centre screen while charging the AOE heal).
    if (role_ == PlayerRole::Cleric && heal_charge_fx_ > 0.001f) {
        const f32 frac = glm::clamp(heal_charge_fx_ / kHealChargeTime, 0.0f, 1.0f);
        const f32 cw = std::min(360.0f, W * 0.32f);
        const f32 cx = (W - cw) * 0.5f;
        const f32 cy = H * 0.62f;
        hud::bar(draw, Vec4{cx, cy, cw, ts * 0.8f}, frac, Vec4{0.7f, 1.0f, 0.82f, 1.0f},
                 Vec4{0.3f, 0.78f, 0.52f, 1.0f}, 5);
        hud::text(draw, Vec2{W * 0.5f, cy - ts * 1.0f}, "CHANNELLING HEAL...", ts * 0.68f,
                  Vec4{0.75f, 1.0f, 0.86f, 1.0f}, ui::TextAlign::Center);
    }

    // Mage combo casting: while holding Ctrl, show the queued elements + the spell they'll cast,
    // plus a hint. Element colours: fire/water/earth/nature.
    if (role_ == PlayerRole::Mage && (casting_ || combo_n_ > 0)) {
        static const Vec4 ecol[4] = {{1.0f, 0.5f, 0.2f, 1.0f},
                                     {0.4f, 0.65f, 1.0f, 1.0f},
                                     {0.6f, 0.45f, 0.3f, 1.0f},
                                     {0.45f, 0.85f, 0.45f, 1.0f}};
        const f32 sz = ts * 1.8f, gap = ts * 0.55f;
        const f32 total = kMaxCombo * sz + (kMaxCombo - 1) * gap;
        const f32 bx = (W - total) * 0.5f;
        const f32 by = H * 0.66f;
        hud::text(draw, Vec2{W * 0.5f, by - ts * 1.4f}, "WEAVING SPELL", ts * 0.68f,
                  Vec4{0.84f, 0.74f, 1.0f, 1.0f}, ui::TextAlign::Center, ui::FontFace::Display);
        // Each queued element is a glowing gem in a gold setting; empty sockets wait dark.
        for (int i = 0; i < kMaxCombo; ++i) {
            const f32 sx = bx + static_cast<f32>(i) * (sz + gap);
            const Vec4 r{sx, by, sz, sz};
            const bool filled = i < combo_n_;
            hud::medallion(draw, Vec2{sx + sz * 0.5f, by + sz * 0.5f}, sz * 0.5f - 4.0f,
                           Vec4{0.05f, 0.04f, 0.06f, 1.0f});
            if (filled) {
                const Vec4 c = ecol[combo_[i] & 3];
                draw.glow(Vec4{r.x - sz * 0.3f, r.y - sz * 0.3f, sz * 1.6f, sz * 1.6f}, hud::alpha(c, 0.5f),
                          Vec4{Vec3{c}, 0.0f});
                const f32 g = sz * 0.3f;
                draw.gradient(Vec4{r.x + sz * 0.5f - g, r.y + sz * 0.5f - g, g * 2.0f, g * 2.0f},
                              Vec4{glm::mix(Vec3{c}, Vec3{1.0f}, 0.45f), 1.0f}, Vec4{Vec3{c} * 0.7f, 1.0f}, g);
            }
        }
        const auto spell = static_cast<SpellId>(resolve_combo());
        const char* sn = spell == SpellId::None ? "..." : spell_name(spell);
        hud::text(draw, Vec2{W * 0.5f, by + sz + 10.0f}, sn, ts * 0.88f, Vec4{0.96f, 0.9f, 1.0f, 1.0f},
                  ui::TextAlign::Center, ui::FontFace::Display);
        const char* how = "HOLD [CTRL] + [1]-[4] / [WASD], RELEASE TO CAST";
        hud::rich(draw, Vec2{(W - hud::rich_width(how, ts * 0.52f)) * 0.5f, by + sz + 10.0f + ts * 1.4f}, how,
                  ts * 0.52f, ui::theme().text_muted);
    } else if (role_ == PlayerRole::Mage) {
        // A standing hint so casting is discoverable (the bar keys do something for the Mage too).
        const char* hint = "TAP [1]-[4] TO CAST  -  HOLD [CTRL] FOR COMBOS (EARTH X3 = WALL, FIRE X2 = METEOR)";
        const f32 bar_top = H - glm::clamp(H * 0.092f, 56.0f, 84.0f) - 26.0f;
        const f32 hs2 = ts * 0.5f;
        hud::rich(draw, Vec2{(W - hud::rich_width(hint, hs2)) * 0.5f, bar_top - ts * 1.5f}, hint, hs2,
                  Vec4{0.84f, 0.76f, 1.0f, 0.95f});
    }

    draw_ability_bar(draw, W, H, ts);

    // Damage flash: a red wash over the screen when WE take a hit (revived feedback - it pairs
    // with the hit marker below so both giving and taking damage read instantly).
    if (hit_flash_ > 0.001f) {
        // A red vignette bleeding in from the screen edges (the oversized ellipse puts the corners
        // inside it), so the hit reads without washing out the fight in the middle.
        draw.glow(Vec4{-W * 0.3f, -H * 0.3f, W * 1.6f, H * 1.6f}, Vec4{0.85f, 0.1f, 0.08f, 0.0f},
                  Vec4{0.85f, 0.1f, 0.08f, hit_flash_ * 0.9f}, 0.45f);
    }
    // Hit marker: a crisp screen-centre X that pops + fades whenever one of OUR attacks lands a
    // confirmed hit (any role / any attack). Punchy white, briefly punching outward as it fades.
    if (hit_marker_ > 0.001f) {
        const f32 m = hit_marker_;
        const Vec2 c{W * 0.5f, H * 0.5f};
        const f32 r0 = 7.0f + (1.0f - m) * 6.0f; // punches outward as it fades
        const f32 r1 = r0 + 10.0f;
        const f32 thick = 3.5f;
        const Vec4 mark_col{1.0f, 0.97f, 0.9f, glm::clamp(m * 1.1f, 0.0f, 1.0f)};
        const Vec4 mark_shadow{0.0f, 0.0f, 0.0f, glm::clamp(m * 0.6f, 0.0f, 1.0f)};
        const Vec2 diag[4] = {{0.707f, 0.707f}, {-0.707f, 0.707f}, {0.707f, -0.707f}, {-0.707f, -0.707f}};
        for (const Vec2& d : diag) {
            const Vec2 pa = c + d * r0;
            const Vec2 pb = c + d * r1;
            draw.line(pa + Vec2{1.5f, 1.5f}, pb + Vec2{1.5f, 1.5f}, thick + 1.5f, mark_shadow); // shadow
            draw.line(pa, pb, thick, mark_col);
        }
    }

    draw_journey_guide(draw, W, H); // the next goal's marker + the town arrival banner
    draw_progress_fx(draw, W, H);   // LEVEL UP / JOURNEY STEP banners

    // NPC pathfinding routes (toggled via the F1 overlay's NPC PATHS button / F4). Drawn whether or
    // not the panel is open, so you can switch it on and watch the routes while playing.
    draw_nav_paths(draw, W, H);

    // Debug / testing overlay on top of everything (F1).
    if (debug_open_) {
        draw_debug(draw, H);
    }
}

void ClientApp::draw_charge_meter(ui::DrawList& draw, f32 W, f32 H) {
    if (!attack_held_ || charge_ <= 0.0f) {
        return;
    }
    Vec2 sp;
    if (!world_to_screen(local_feet() - Vec3{0.0f, 0.15f, 0.0f}, W, H, sp)) {
        return;
    }
    // A short gilded gauge under the hero, filling in their role's colour; full, it flares white-gold
    // and calls for the release.
    const bool full = charge_ >= kFullCharge;
    const Vec3 rc = role_color(role_);
    const f32 pulse = full ? 0.75f + 0.25f * std::sin(elapsed_ * 18.0f) : 1.0f;
    const Vec3 fill = full ? glm::mix(rc, Vec3{1.0f, 0.95f, 0.75f}, 0.6f) : rc;
    const f32 bw = 92.0f, bh = 9.0f;
    const Vec4 r{sp.x - bw * 0.5f, sp.y + 16.0f, bw, bh};
    if (full) {
        draw.shadow(r, 4.5f, 12.0f, Vec4{fill, 0.55f * pulse});
    }
    hud::bar(draw, r, charge_, Vec4{glm::mix(fill, Vec3{1.0f}, 0.25f) * pulse, 1.0f}, Vec4{fill * 0.7f * pulse, 1.0f}, 4);
    hud::text(draw, Vec2{sp.x, r.y + bh + 5.0f}, full ? "RELEASE!" : "HEAVY", 12.0f,
              full ? Vec4{1.0f, 0.93f, 0.7f, pulse} : hud::alpha(ui::theme().text, 0.85f), ui::TextAlign::Center);
}

void ClientApp::draw_nav_paths(ui::DrawList& draw, f32 W, f32 H) {
    // Only meaningful on a listen server we host: we own the sim, so we have the live path data; a
    // remote client has none (the routes aren't networked).
    if (!debug_paths_ || !host_local_ || !local_server_.running()) {
        return;
    }
    // Project a world point through the iso camera. Returns false when the point is behind the camera
    // (so a segment with an endpoint behind us is skipped); off-screen-but-in-front points still
    // project, so a line just runs off the screen edge.
    const Mat4 vp = camera_.view_projection();
    auto project = [&](const Vec3& wp, Vec2& sp) -> bool {
        const Vec4 clip = vp * Vec4{wp, 1.0f};
        if (clip.w <= 0.05f) {
            return false;
        }
        sp = Vec2{(clip.x / clip.w * 0.5f + 0.5f) * W, (clip.y / clip.w * 0.5f + 0.5f) * H};
        return true;
    };
    const Vec4 kind_col[4] = {
        {0.25f, 0.95f, 1.0f, 0.95f}, // 0 teamster A* path  - cyan (the real around-obstacles route)
        {1.0f, 0.82f, 0.2f, 0.9f},   // 1 wagon road route  - gold
        {0.45f, 0.9f, 0.5f, 0.7f},   // 2 villager/guard goal - green
        {1.0f, 0.4f, 0.32f, 0.8f},   // 3 ambusher goal     - red
    };
    // The server ticks on its own thread; take the lock while copying its path data out.
    std::vector<GameServer::DebugNavPath> paths;
    {
        std::lock_guard<std::mutex> lock(server_mutex_);
        paths = local_server_.debug_nav_paths();
    }
    for (const GameServer::DebugNavPath& path : paths) {
        const Vec4 col = kind_col[path.kind % 4];
        const f32 thick = path.kind <= 1 ? 2.6f : 1.6f; // emphasise the A* path + the wagon route
        Vec2 prev{};
        bool have_prev = false;
        for (const Vec3& wp : path.points) {
            Vec2 sp{};
            const bool on = project(wp, sp);
            if (on && have_prev) {
                draw.line(prev, sp, thick, col);
            }
            if (on && path.kind <= 1) {
                draw.rect(Vec4{sp.x - 2.5f, sp.y - 2.5f, 5.0f, 5.0f}, col, 1.5f); // waypoint node
            }
            prev = sp;
            have_prev = on;
        }
    }
}

void ClientApp::draw_debug(ui::DrawList& draw, f32 H) {
    const f32 ds = glm::clamp(H * 0.02f, 13.0f, 22.0f); // debug text size
    const f32 pad = 12.0f;
    const f32 pw = std::max(290.0f, draw.text_width("NO WAGON ATTACKS:  OFF", ds) + pad * 2.0f);
    const f32 x = 16.0f;
    const f32 y = H * 0.26f;
    const f32 line = ds * 1.32f;
    const bool host = host_local_ && local_server_.running();
    const int rows = 10; // title + 6 metrics + 3 toggles
    const f32 ph = pad * 2.0f + line * static_cast<f32>(rows) + line * 1.4f; // + hint
    // Panel.
    draw.rect(Vec4{x, y, pw, ph}, Vec4{0.04f, 0.05f, 0.07f, 0.82f},
              Vec4{0.5f, 0.62f, 0.78f, 0.9f}, 1.5f, 7.0f);
    f32 ty = y + pad;
    draw.text(Vec2{x + pad, ty}, "DEBUG  [F1]", ds * 1.05f, Vec4{0.7f, 0.85f, 1.0f, 1.0f});
    ty += line * 1.2f;
    auto metric = [&](const std::string& s, const Vec4& c) {
        draw.text(Vec2{x + pad, ty}, s, ds, c);
        ty += line;
    };
    const Vec4 mc{0.82f, 0.88f, 0.95f, 1.0f};
    const f32 fps = fps_;
    const Vec4 fps_c = fps >= 55.0f ? Vec4{0.6f, 0.95f, 0.65f, 1.0f}
                       : fps >= 30.0f ? Vec4{0.95f, 0.85f, 0.45f, 1.0f}
                                      : Vec4{0.95f, 0.45f, 0.4f, 1.0f};
    metric(std::format("FPS         {:.0f}  ({:.1f} ms)", fps, frame_ms_), fps_c);
    metric(std::format("SERVER TPS  {:.0f}", server_tps_), mc);
    metric(std::format("PLAYERS     {}", snapshot_.players.size()), mc);
    metric(std::format("ENEMIES     {}", snapshot_.enemies.size()), mc);
    metric(std::format("PROJECTILES {}", snapshot_.projectiles.size()), mc);
    metric(std::format("PARTICLES   {}", particles_.size()), mc);

    // Two clickable toggles.
    auto toggle = [&](const char* label, bool on, ui::Rect& rect) {
        const f32 bx = x + pad;
        const f32 bw = pw - pad * 2.0f;
        const f32 bh = line * 1.05f;
        rect = ui::Rect{bx, ty, bw, bh};
        const Vec4 fill = on ? Vec4{0.18f, 0.42f, 0.22f, 0.95f} : Vec4{0.16f, 0.16f, 0.2f, 0.9f};
        const Vec4 border = on ? Vec4{0.5f, 0.95f, 0.55f, 1.0f} : Vec4{0.45f, 0.48f, 0.55f, 0.9f};
        draw.rect(Vec4{bx, ty, bw, bh}, fill, border, 1.5f, 5.0f);
        draw.text(Vec2{bx + 8.0f, ty + bh * 0.5f - ds * 0.5f}, label, ds, Vec4{0.85f, 0.9f, 0.95f, 1.0f});
        const std::string st = on ? "ON" : "OFF";
        draw.text(Vec2{bx + bw - draw.text_width(st, ds) - 10.0f, ty + bh * 0.5f - ds * 0.5f}, st, ds,
                  on ? Vec4{0.6f, 1.0f, 0.65f, 1.0f} : Vec4{0.7f, 0.72f, 0.78f, 1.0f});
        ty += bh + 4.0f;
    };
    toggle("GODMODE  [F2]", debug_god_, god_btn_);
    toggle("NO WAGON ATTACKS  [F3]", debug_no_ambush_, noatk_btn_);
    toggle("NPC PATHS  [F4]", debug_paths_, npc_paths_btn_);
    if (!host) {
        draw.text(Vec2{x + pad, ty}, "(toggles need a hosted game)", ds * 0.82f,
                  Vec4{0.85f, 0.6f, 0.45f, 0.95f});
    }
}

void ClientApp::draw_contract_panel(ui::DrawList& draw, const net::WagonState& wg, bool accepted, f32 W, f32 H, f32 ts, const Vec3& feet) {
    const f32 pw = glm::clamp(W * 0.24f, 280.0f, 380.0f);
    // Tall enough for the header, three detail rows, the optional modifier chip, the mode hint
    // and the buttons.
    const bool has_mod = contract_modifier(wg.id) != ContractModifier::Standard;
    const f32 ph = ts * (has_mod ? 12.9f : 11.6f);
    // Anchor beside the wagon on screen; clamp on-screen, fall back to centre if off-camera.
    Vec2 sp;
    Vec2 anchor{(W - pw) * 0.5f, H * 0.26f};
    if (world_to_screen(wg.position + Vec3{0.0f, 2.6f, 0.0f}, W, H, sp)) {
        anchor = Vec2{sp.x - pw * 0.5f, sp.y - ph - 12.0f};
    }
    anchor.x = glm::clamp(anchor.x, 12.0f, W - pw - 12.0f);
    anchor.y = glm::clamp(anchor.y, 12.0f, H - ph - 120.0f);
    const f32 px = anchor.x, py = anchor.y;

    const ui::Theme& th = ui::theme();
    // A gold-bound card with a tail pointing down at its wagon.
    const Vec4 card{px, py, pw, ph};
    draw.shadow(card, 12.0f, 18.0f, Vec4{0.0f, 0.0f, 0.0f, 0.6f}, Vec2{0.0f, 6.0f});
    draw.gradient(card, th.panel, th.panel_bottom, 12.0f, th.panel_border, 1.75f);
    draw.outline(Vec4{px + 5.0f, py + 5.0f, pw - 10.0f, ph - 10.0f}, hud::alpha(th.accent, 0.3f), 1.0f, 8.0f);
    const f32 ix = px + 20.0f;
    const f32 right = px + pw - 20.0f;
    f32 iy = py + 16.0f;

    // Heading: the kind of job, then the bound-for town in display capitals. An enclosed
    // (tall-walled) bed carries the NOBLE - the same rule generate_offers uses for Passengers
    // cargo - and reads as a premium VIP escort.
    const bool vip = vehicle_type(wg.type).bed().wall > 2.0f;
    hud::text(draw, Vec2{ix, iy}, vip ? "VIP ESCORT - THE NOBLE'S CARRIAGE" : "CARGO CONTRACT",
              ts * 0.56f, vip ? hud::kWarn : th.text_muted);
    iy += ts * 1.05f;
    hud::heading(draw, Vec2{ix, iy}, std::format("TO {}", town_name(Vec3{wg.dest.x, 0.0f, wg.dest.z})),
                 ts * 1.05f);
    iy += ts * 1.65f;
    draw.line(Vec2{ix, iy}, Vec2{right, iy}, 1.0f, hud::alpha(th.accent, 0.4f));
    iy += ts * 0.55f;

    // Distance + danger + pay, as label / value rows.
    auto row_label = [&](const char* label) { hud::text(draw, Vec2{ix, iy}, label, ts * 0.6f, th.text_muted); };
    const f32 dist = glm::length(Vec2{wg.dest.x - feet.x, wg.dest.z - feet.z});
    row_label("DISTANCE");
    hud::text(draw, Vec2{right, iy - ts * 0.08f}, std::format("~{} M", static_cast<int>(dist)), ts * 0.72f,
              th.text, ui::TextAlign::Right);
    iy += ts * 1.2f;
    const char* danger = wg.difficulty <= 1 ? "LOW" : wg.difficulty == 2 ? "MODERATE" : "HIGH";
    const Vec4 dcol = wg.difficulty <= 1 ? hud::kGood : wg.difficulty == 2 ? hud::kWarn : hud::kBad;
    row_label("DANGER");
    {
        // Danger pips (1..3) left of the rating.
        const f32 dw = hud::width(danger, ts * 0.72f);
        const int dd = glm::clamp<int>(wg.difficulty, 1, 3);
        for (int k = 0; k < 3; ++k) {
            const f32 pr = ts * 0.22f;
            const Vec2 pc{right - dw - ts * 0.5f - static_cast<f32>(2 - k) * pr * 2.6f - pr, iy + ts * 0.3f};
            draw.rect(Vec4{pc.x - pr, pc.y - pr, pr * 2.0f, pr * 2.0f},
                      k < dd ? dcol : Vec4{0.2f, 0.15f, 0.1f, 0.9f}, pr);
            draw.outline(Vec4{pc.x - pr, pc.y - pr, pr * 2.0f, pr * 2.0f}, hud::kInk, 1.0f, pr);
        }
        hud::text(draw, Vec2{right, iy - ts * 0.08f}, danger, ts * 0.72f, dcol, ui::TextAlign::Right);
    }
    iy += ts * 1.2f;
    row_label("PAY");
    {
        const std::string pay = std::format("{}", wg.reward);
        const f32 pwid = hud::width(pay, ts * 0.9f);
        hud::coin(draw, Vec2{right - pwid - ts * 0.55f, iy + ts * 0.32f}, ts * 0.36f);
        hud::text(draw, Vec2{right, iy - ts * 0.2f}, pay, ts * 0.9f, hud::kGold, ui::TextAlign::Right);
    }
    // A per-contract modifier (derived from the wagon id) - hazardous / bulk / safe runs vary the
    // pay + ambush so the board isn't all the same; standard runs show nothing.
    if (const ContractModifier mod = contract_modifier(wg.id); mod != ContractModifier::Standard) {
        iy += ts * 1.35f;
        const Vec4 mcol = mod == ContractModifier::Safe   ? Vec4{0.6f, 0.85f, 0.95f, 1.0f}
                          : mod == ContractModifier::Bulk ? Vec4{0.82f, 0.74f, 0.96f, 1.0f}
                                                          : Vec4{0.98f, 0.55f, 0.45f, 1.0f};
        hud::chip(draw, Vec2{ix, iy - ts * 0.15f}, modifier_name(mod), ts * 0.56f, mcol);
    }

    // Mode hint (toggled with H).
    const char* mode = vote_mode_ == 2 ? "[H] HAUL MANUALLY (+PAY)" : "[H] HIRE A DRIVER";
    hud::rich(draw, Vec2{ix, py + ph - ts * 3.05f}, mode, ts * 0.56f, th.text);

    // Buttons: a green-gold ACCEPT plaque (or the vote tally once accepted) and a dark CANCEL.
    const f32 bw = (pw - 20.0f * 2.0f - 12.0f) * 0.5f;
    const f32 bh = ts * 1.7f;
    const f32 by = py + ph - bh - 14.0f;
    const Vec2 mouse = pointer_pos();
    auto button = [&](const ui::Rect& r, const char* label, Vec3 base, Vec4 text_col) {
        const bool hot = in_rect(mouse, r);
        const Vec4 rr{r.x, r.y, r.w, r.h};
        draw.shadow(rr, 7.0f, 5.0f, Vec4{0.0f, 0.0f, 0.0f, 0.55f}, Vec2{0.0f, 2.5f});
        draw.gradient(rr, Vec4{base * (hot ? 1.45f : 1.25f), 1.0f}, Vec4{base * (hot ? 0.95f : 0.8f), 1.0f}, 7.0f,
                      hud::alpha(th.accent_hover, hot ? 0.95f : 0.6f), 1.5f);
        draw.line(Vec2{rr.x + 7.0f, rr.y + 2.0f}, Vec2{rr.x + rr.z - 7.0f, rr.y + 2.0f}, 1.0f,
                  Vec4{1.0f, 0.95f, 0.85f, 0.18f});
        hud::text(draw, Vec2{rr.x + rr.z * 0.5f, rr.y + (rr.w - ts * 0.7f) * 0.5f}, label, ts * 0.7f, text_col,
                  ui::TextAlign::Center);
    };
    if (accepted) {
        const int total = static_cast<int>(snapshot_.players.size());
        const Vec4 wr{ix, by, bw, bh};
        draw.gradient(wr, Vec4{0.0f, 0.0f, 0.0f, 0.35f}, Vec4{0.0f, 0.0f, 0.0f, 0.2f}, 7.0f,
                      hud::alpha(hud::kGood, 0.5f), 1.0f);
        hud::text(draw, Vec2{wr.x + bw * 0.5f, by + (bh - ts * 0.6f) * 0.5f},
                  std::format("WAITING  {} / {}", wg.votes, total), ts * 0.6f, hud::kGood, ui::TextAlign::Center);
        cancel_btn_ = ui::Rect{ix + bw + 12.0f, by, bw, bh};
        button(cancel_btn_, "CANCEL", Vec3{0.30f, 0.12f, 0.09f}, Vec4{1.0f, 0.86f, 0.8f, 1.0f});
    } else {
        accept_btn_ = ui::Rect{ix, by, bw, bh};
        button(accept_btn_, "ACCEPT", Vec3{0.20f, 0.32f, 0.13f}, Vec4{0.9f, 1.0f, 0.82f, 1.0f});
        cancel_btn_ = ui::Rect{ix + bw + 12.0f, by, bw, bh};
        button(cancel_btn_, "CANCEL", Vec3{0.20f, 0.145f, 0.095f}, th.text);
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

    // Backing plaque, with a stud at each end like a bound strap.
    const Vec4 back{x0 - pad, sy - pad, total + pad * 2.0f, slot + pad * 2.0f};
    hud::plaque(draw, back, 0.86f, 12.0f);
    hud::stud(draw, Vec2{back.x + pad * 0.5f, back.y + back.w * 0.5f}, 3.0f);
    hud::stud(draw, Vec2{back.x + back.z - pad * 0.5f, back.y + back.w * 0.5f}, 3.0f);
    const ui::Theme& th = ui::theme();

    const f32 r = slot * 0.16f;
    const f32 ks = slot * 0.17f; // key-cap text size
    f32 sx = x0;
    for (u8 i = 0; i < kAbilitySlots; ++i) {
        ability_slot_rects_[i] = ui::Rect{sx, sy, slot, slot}; // cached for drag hit-testing
        const int a = bar_[i];
        const bool empty = a < 0;
        const bool dragging = drag_slot_ == static_cast<int>(i);
        const Vec4 sr{sx, sy, slot, slot};

        if (empty) {
            // An empty socket: a dark recess + a faded key cap (drop a skill here).
            draw.gradient(sr, Vec4{0.02f, 0.015f, 0.01f, 0.8f}, Vec4{0.07f, 0.05f, 0.035f, 0.8f}, r,
                          hud::alpha(th.accent, 0.25f), 1.25f);
            hud::text(draw, Vec2{sx + slot * 0.5f, sy + (slot - ks * 1.4f) * 0.5f}, std::format("{}", i + 1),
                      ks * 1.4f, hud::alpha(th.text_muted, 0.4f), ui::TextAlign::Center);
            sx += slot + gap;
            continue;
        }

        if (!knows(known_mask(), static_cast<u8>(a))) {
            // Not learned yet (a Mage's locked element): a dim socket with the faded icon + a padlock.
            draw.gradient(sr, Vec4{0.04f, 0.03f, 0.02f, 0.9f}, Vec4{0.08f, 0.06f, 0.04f, 0.9f}, r,
                          hud::alpha(th.accent, 0.3f), 1.25f);
            draw_ability_icon(draw, role_, static_cast<u8>(a), sx + slot * 0.5f, sy + slot * 0.52f, slot * 0.26f,
                              Vec4{0.4f, 0.36f, 0.32f, 0.6f});
            const Vec2 lc{sx + slot * 0.72f, sy + slot * 0.72f};
            const f32 ls = slot * 0.13f;
            draw.line(Vec2{lc.x - ls * 0.32f, lc.y - ls * 0.1f}, Vec2{lc.x - ls * 0.32f, lc.y - ls * 0.42f}, ls * 0.2f, th.text_muted);
            draw.line(Vec2{lc.x + ls * 0.32f, lc.y - ls * 0.1f}, Vec2{lc.x + ls * 0.32f, lc.y - ls * 0.42f}, ls * 0.2f, th.text_muted);
            draw.line(Vec2{lc.x - ls * 0.32f, lc.y - ls * 0.42f}, Vec2{lc.x + ls * 0.32f, lc.y - ls * 0.42f}, ls * 0.2f, th.text_muted);
            draw.rect(Vec4{lc.x - ls * 0.55f, lc.y - ls * 0.1f, ls * 1.1f, ls * 0.85f}, th.text_muted, ls * 0.15f);
            hud::key_cap(draw, Vec2{sx - ks * 0.35f, sy - ks * 0.2f}, std::format("{}", i + 1), ks);
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

        // Slot face: a recessed tile with a soft glow of the role colour behind the icon, in a gold
        // frame that brightens (with a warm halo) while the ability is ready.
        if (ready) {
            draw.shadow(sr, r, 7.0f, hud::alpha(th.accent_hover, 0.35f));
        }
        draw.gradient(sr, Vec4{0.13f, 0.095f, 0.065f, 0.97f}, Vec4{0.05f, 0.035f, 0.025f, 0.97f}, r,
                      ready ? th.accent_hover : hud::alpha(th.accent, 0.45f), ready ? 2.0f : 1.25f);
        draw.glow(Vec4{sx + slot * 0.12f, sy + slot * 0.12f, slot * 0.76f, slot * 0.76f},
                  Vec4{accent, ready ? 0.30f : 0.10f}, Vec4{accent, 0.0f});

        // Icon, tinted by readiness (centred - the bar is icon-only; names live in the skills tree).
        const Vec4 icol{ready ? glm::mix(accent, Vec3{1.0f}, 0.2f) : accent * 0.45f, ready ? 1.0f : 0.7f};
        draw_ability_icon(draw, role_, static_cast<u8>(a), sx + slot * 0.5f, sy + slot * 0.52f,
                          slot * 0.26f, icol);

        // Cooldown: a dark curtain draining from the top, its edge lit, + the seconds remaining.
        if (!ready) {
            draw.rect(Vec4{sx, sy, slot, slot * frac}, Vec4{0.02f, 0.015f, 0.01f, 0.66f}, r);
            draw.line(Vec2{sx + 3.0f, sy + slot * frac}, Vec2{sx + slot - 3.0f, sy + slot * frac}, 1.5f,
                      hud::alpha(th.accent_hover, 0.7f));
            hud::text(draw, Vec2{sx + slot * 0.5f, sy + (slot - ts) * 0.5f},
                      std::format("{:.0f}", std::ceil(cd_now)), ts, Vec4{1.0f, 0.97f, 0.9f, 1.0f},
                      ui::TextAlign::Center);
        }
        // The slot being click-dragged is dimmed; its icon follows the cursor (drawn below).
        if (dragging) {
            draw.rect(sr, Vec4{0.03f, 0.02f, 0.015f, 0.6f}, r);
        }

        // Its key, as a key cap on the top-left corner.
        hud::key_cap(draw, Vec2{sx - ks * 0.35f, sy - ks * 0.2f}, std::format("{}", i + 1), ks);
        sx += slot + gap;
    }

    // The dragged ability rides under the cursor while reordering.
    if (drag_slot_ >= 0 && bar_[drag_slot_] >= 0) {
        const Vec2 p = pointer_pos();
        const Vec4 dr{p.x - slot * 0.5f, p.y - slot * 0.5f, slot, slot};
        draw.shadow(dr, r, 10.0f, Vec4{0.0f, 0.0f, 0.0f, 0.6f}, Vec2{0.0f, 6.0f});
        draw.gradient(dr, Vec4{0.13f, 0.095f, 0.065f, 0.92f}, Vec4{0.05f, 0.035f, 0.025f, 0.92f}, r,
                      th.accent_hover, 2.0f);
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
    // A compass medallion at the top centre whose gold needle points to the destination.
    const f32 R = 26.0f;
    const Vec2 c{W * 0.5f, 22.0f + R};
    hud::medallion(draw, c, R, Vec4{0.07f, 0.05f, 0.035f, 0.95f});
    const Vec4 gold = hud::kGold;
    const Vec4 ink{0.05f, 0.03f, 0.015f, 1.0f};
    const f32 L = R * 0.72f, hw = R * 0.3f;
    const Vec2 tip = c + d * L;
    for (const auto& [w, col] : {std::pair{7.5f, ink}, std::pair{4.5f, gold}}) { // inked outline, then gold
        draw.line(c - d * L * 0.55f, tip, w, col);
        draw.line(tip, tip - d * hw + perp * hw, w, col);
        draw.line(tip, tip - d * hw - perp * hw, w, col);
    }
    hud::stud(draw, c, 3.0f);
    hud::text(draw, Vec2{c.x, c.y + R + 12.0f}, "DESTINATION", 13.0f, gold, ui::TextAlign::Center,
              ui::FontFace::Display);
}

// An always-on corner minimap: a small north-up window around the player showing nearby roads, the
// active haul's destination (clamped to the edge if off-window), enemy threats, and the player at
// centre with a facing tick. Cheap (lines + dots, no terrain raster) - the full M map stays the
// detailed pannable view.
void ClientApp::draw_minimap(ui::DrawList& draw, const Vec3& feet, f32 W, f32 H) {
    const f32 sz = glm::clamp(H * 0.2f, 132.0f, 200.0f);
    // Top-right, just below the money pill: a round medallion with a "N" marker at the top.
    const f32 top = 16.0f + glm::clamp(H * 0.026f, 15.0f, 30.0f) * 1.9f + 16.0f;
    const Vec4 box{W - sz - 22.0f, top, sz, sz};
    const Vec2 c{box.x + sz * 0.5f, box.y + sz * 0.5f};
    constexpr f32 mm_radius = 120.0f; // world metres from centre to edge
    const f32 scale = (sz * 0.5f) / mm_radius;
    hud::medallion(draw, c, sz * 0.5f, Vec4{0.13f, 0.15f, 0.1f, 0.78f});
    draw.glow(Vec4{box.x, box.y, sz, sz}, Vec4{0.36f, 0.42f, 0.24f, 0.35f}, Vec4{0.0f, 0.0f, 0.0f, 0.55f});

    const Vec2 pxz{feet.x, feet.z};
    auto to_mm = [&](Vec2 wxz) { return c + Vec2{(wxz.x - pxz.x) * scale, (wxz.y - pxz.y) * scale}; };
    auto inside = [&](const Vec2& p) { return glm::length(p - c) <= sz * 0.5f - 4.0f; };

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
    // The side quest's site: a pale-blue diamond (edge-clamped, like the wagon's destination).
    if (const net::QuestState* q = active_quest()) {
        Vec2 d = to_mm(Vec2{q->site.x, q->site.z});
        const f32 r = sz * 0.5f - 7.0f;
        if (const Vec2 off = d - c; glm::length(off) > r) {
            d = c + glm::normalize(off) * r;
        }
        const Vec4 qc{0.62f, 0.86f, 1.0f, 1.0f};
        draw.line(Vec2{d.x, d.y - 6.0f}, Vec2{d.x + 6.0f, d.y}, 3.0f, qc);
        draw.line(Vec2{d.x + 6.0f, d.y}, Vec2{d.x, d.y + 6.0f}, 3.0f, qc);
        draw.line(Vec2{d.x, d.y + 6.0f}, Vec2{d.x - 6.0f, d.y}, 3.0f, qc);
        draw.line(Vec2{d.x - 6.0f, d.y}, Vec2{d.x, d.y - 6.0f}, 3.0f, qc);
    }
    // A roadside errand: a gold dot at the traveller (or where the help's needed, once it's under way).
    if (const net::ErrandState* e = current_errand(); e != nullptr && e->phase != static_cast<u8>(QuestPhase::Complete)) {
        const Vec3 at = e->phase == static_cast<u8>(QuestPhase::Active) ? e->site : e->giver;
        Vec2 d = to_mm(Vec2{at.x, at.z});
        const f32 r = sz * 0.5f - 7.0f;
        if (const Vec2 off = d - c; glm::length(off) > r) {
            d = c + glm::normalize(off) * r;
        }
        draw.rect(Vec4{d.x - 5.0f, d.y - 5.0f, 10.0f, 10.0f}, Vec4{0.08f, 0.05f, 0.02f, 0.9f}, 5.0f);
        draw.rect(Vec4{d.x - 3.5f, d.y - 3.5f, 7.0f, 7.0f}, Vec4{1.0f, 0.88f, 0.5f, 1.0f}, 3.5f);
    }
    // Teammates, each in their identity colour (edge-clamped so you can always find them).
    for (const net::PlayerState& p : snapshot_.players) {
        if (p.id == my_id_) {
            continue;
        }
        Vec2 d = to_mm(Vec2{p.position.x, p.position.z});
        const f32 r = sz * 0.5f - 6.0f;
        if (const Vec2 off = d - c; glm::length(off) > r) {
            d = c + glm::normalize(off) * r;
        }
        draw.rect(Vec4{d.x - 5.0f, d.y - 5.0f, 10.0f, 10.0f}, Vec4{0.05f, 0.03f, 0.02f, 0.9f}, 5.0f);
        draw.rect(Vec4{d.x - 3.5f, d.y - 3.5f, 7.0f, 7.0f}, Vec4{player_color(p.color), 1.0f}, 3.5f);
    }
    // The player at centre + a heading tick.
    const Vec2 fwd{std::cos(face_yaw_), std::sin(face_yaw_)};
    draw.line(c, c + fwd * 12.0f, 4.5f, Vec4{0.05f, 0.03f, 0.02f, 0.9f});
    draw.line(c, c + fwd * 11.0f, 2.5f, Vec4{1.0f, 0.97f, 0.9f, 1.0f});
    draw.rect(Vec4{c.x - 4.5f, c.y - 4.5f, 9.0f, 9.0f}, Vec4{0.05f, 0.03f, 0.02f, 0.9f}, 4.5f);
    draw.rect(Vec4{c.x - 3.0f, c.y - 3.0f, 6.0f, 6.0f}, Vec4{glm::mix(player_color(live_color_), Vec3{1.0f}, 0.3f), 1.0f}, 3.0f);
    // North, on the rim.
    const Vec2 n{c.x, box.y + 2.0f};
    draw.rect(Vec4{n.x - 9.0f, n.y - 9.0f, 18.0f, 18.0f}, Vec4{0.08f, 0.05f, 0.03f, 1.0f}, 9.0f);
    draw.outline(Vec4{n.x - 9.0f, n.y - 9.0f, 18.0f, 18.0f}, hud::kGold, 1.5f, 9.0f);
    hud::text(draw, Vec2{n.x, n.y - 5.5f}, "N", 11.0f, hud::kGold, ui::TextAlign::Center,
              ui::FontFace::Display);
}

void ClientApp::rebuild_map_raster(const Vec4& panel, f32 ppm) {
    map_tiles_.clear();
    const Vec2 mc{panel.x + panel.z * 0.5f, panel.y + panel.w * 0.5f};
    const f32 top = panel.y + kMapInset, left = panel.x + kMapInset;
    const f32 right = panel.x + panel.z - kMapInset, bot = panel.y + panel.w - kMapInset;
    // Raster an overscan margin beyond the visible area, so draw_map's screen-space transform
    // can pan / gently zoom the cached raster between rebuilds without exposing bare panel at
    // the edges (the visible window is scissored back to the panel).
    const f32 over = std::min(panel.z, panel.w) * 0.16f;
    const f32 rx0 = left - over, ry0 = top - over;
    const f32 area_w = (right + over) - rx0, area_h = (bot + over) - ry0;
    // Fine ~6 px cells: the raster is drawn as ONE instanced call (Renderer::draw_ui_tiles),
    // so the cell size is a worldgen-sampling budget, not a draw-call budget - the old
    // draw-call-per-rect path is what capped the map at chunky ~16 px tiles.
    const int cols = glm::clamp(static_cast<int>(area_w / 6.0f), 60, 400);
    const f32 cell = area_w / static_cast<f32>(cols);
    const int rows = std::max(1, static_cast<int>(std::ceil(area_h / cell)));
    const u32 seed = world_seed_;

    // Sample the height field ONCE per tile centre (plus one extra row/column so every tile
    // has forward neighbours for its gradient) and share the samples between the relief
    // shading, the water/beach bands and the biome classification. The old per-tile colour
    // re-evaluated height() ~9 times (gradient + biome_at + its slope/temperature) - cutting
    // that is what makes the finer raster affordable.
    const f32 wstep = cell / ppm; // world metres per cell
    const int gw = cols + 1;
    std::vector<f32> hgrid(static_cast<usize>(gw) * static_cast<usize>(rows + 1));

    // The worldgen fields are pure functions (the terrain mesher already samples them from its
    // worker thread concurrently with the main thread), so the raster rows fan out across a few
    // threads - this keeps the occasional rebuild a small blip instead of a Debug-build hitch.
    auto parallel_rows = [](int count, auto&& fn) {
        const u32 workers =
            std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        if (workers <= 1 || count < 32) {
            for (int j = 0; j < count; ++j) {
                fn(j);
            }
            return;
        }
        std::atomic<int> next{0};
        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (u32 t = 0; t < workers; ++t) {
            pool.emplace_back([&] {
                for (int j = next.fetch_add(1); j < count; j = next.fetch_add(1)) {
                    fn(j);
                }
            });
        }
        for (std::thread& th : pool) {
            th.join();
        }
    };

    parallel_rows(rows + 1, [&](int j) {
        for (int i = 0; i <= cols; ++i) {
            const f32 wx =
                map_center_.x + (rx0 + (static_cast<f32>(i) + 0.5f) * cell - mc.x) / ppm;
            const f32 wz =
                map_center_.y + (ry0 + (static_cast<f32>(j) + 0.5f) * cell - mc.y) / ppm;
            // The natural land (the map has no use for the levelled house pads, and skipping
            // them keeps the raster from building town layouts across the whole view).
            hgrid[static_cast<usize>(j) * gw + i] = worldgen::base_height(wx, wz, seed);
        }
    });

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

    // A top-down terrain colour with relief hill-shading, so the map reads as the real
    // landscape: ocean/shallows by depth, beach, then the surface palette, lit by a soft
    // directional shade computed from the shared height-grid gradient. Rows write into
    // pre-sized storage so they can run in parallel.
    map_tiles_.resize(static_cast<usize>(cols) * static_cast<usize>(rows));
    parallel_rows(rows, [&](int j) {
        for (int i = 0; i < cols; ++i) {
            const f32 rx = rx0 + static_cast<f32>(i) * cell;
            const f32 ry = ry0 + static_cast<f32>(j) * cell;
            const f32 wx = map_center_.x + (rx + cell * 0.5f - mc.x) / ppm;
            const f32 wz = map_center_.y + (ry + cell * 0.5f - mc.y) / ppm;
            const f32 h = hgrid[static_cast<usize>(j) * gw + i];
            Vec4 col;
            if (h < worldgen::water_level) {
                const f32 depth = glm::clamp((worldgen::water_level - h) / 8.0f, 0.0f, 1.0f);
                col = Vec4{glm::mix(Vec3{0.24f, 0.46f, 0.56f}, Vec3{0.04f, 0.12f, 0.28f}, depth),
                           1.0f};
            } else {
                const f32 hx = hgrid[static_cast<usize>(j) * gw + i + 1];
                const f32 hz = hgrid[static_cast<usize>(j + 1) * gw + i];
                const Vec3 n = glm::normalize(Vec3{(h - hx) / wstep, 1.0f, (h - hz) / wstep});
                Vec3 c = worldgen::surface_color(Vec3{wx, h, wz}, Vec3{0.0f, 1.0f, 0.0f}, seed);
                // slope() measures |dh| per metre in x + z; reuse the grid gradient for it.
                const f32 sl = (std::abs(hx - h) + std::abs(hz - h)) / wstep;
                const f32 m = worldgen::moisture(wx, wz, seed);
                const f32 t = worldgen::temperature(wx, wz, seed, h);
                c = glm::mix(c, biome_key(worldgen::classify_biome(h, sl, m, t)), 0.42f);
                if (h < worldgen::water_level + 0.7f) {
                    c = glm::mix(c, Vec3{0.80f, 0.74f, 0.54f}, 0.55f); // beach band
                }
                const f32 shade = glm::clamp(
                    glm::dot(n, glm::normalize(Vec3{0.5f, 0.85f, 0.35f})), 0.45f, 1.18f);
                col = Vec4{glm::clamp(c * shade, Vec3{0.0f}, Vec3{1.0f}), 1.0f};
            }
            map_tiles_[static_cast<usize>(j) * cols + i] = {
                Vec4{rx, ry, cell + 1.0f, cell + 1.0f}, col};
        }
    });
    map_raster_center_ = map_center_;
    map_raster_zoom_ = map_zoom_;
    map_raster_ppm_ = ppm;
    map_raster_overscan_ = over;
}

void ClientApp::draw_map() {
    if (renderer_ == nullptr) {
        return;
    }
    const VkExtent2D ext = renderer_->extent();
    const f32 W = static_cast<f32>(ext.width);
    const f32 H = static_cast<f32>(ext.height);
    ui::DrawList draw{*renderer_};
    const ui::Theme& th = ui::theme();

    // The world dimmed behind a framed board; the map raster fills it edge to edge.
    draw.rect(Vec4{0.0f, 0.0f, W, H}, Vec4{0.02f, 0.015f, 0.01f, 0.8f});
    const f32 mg = std::min(W, H) * map::margin_frac;
    const Vec4 panel{mg, mg, W - 2.0f * mg, H - 2.0f * mg};
    draw.shadow(panel, 14.0f, 28.0f, Vec4{0.0f, 0.0f, 0.0f, 0.7f}, Vec2{0.0f, 10.0f});
    draw.gradient(panel, th.panel, th.panel_bottom, 14.0f);

    const f32 inner = std::min(panel.z, panel.w) * 0.5f - 28.0f;
    const f32 view_world = map::view_world / map_zoom_; // zoom in -> smaller span -> more detail
    const f32 ppm = inner / view_world;
    map_ppm_ = ppm;
    const Vec2 mc{panel.x + panel.z * 0.5f, panel.y + panel.w * 0.5f};

    // The cached terrain raster tracks the live view with a cheap screen-space transform
    // (scale about the panel centre + a pan offset), so worldgen is only re-sampled when the
    // view has actually outrun the raster's overscan (urgent) or - rate-limited to every few
    // frames - after any movement. Dragging + zooming therefore stay smooth even though the
    // raster itself is far finer than the old rebuild-every-frame one.
    if (map_raster_cooldown_ > 0) {
        --map_raster_cooldown_;
    }
    const bool ext_changed = ext.width != map_raster_ext_.x || ext.height != map_raster_ext_.y;
    // Zoom + pan drift since the raster was built (the draw-time transform hides it).
    f32 raster_scale = map_raster_zoom_ > 0.0f ? ppm / map_raster_ppm_ : 1.0f;
    Vec2 raster_off = (map_raster_center_ - map_center_) * ppm;
    const bool moved = map_center_ != map_raster_center_ || map_zoom_ != map_raster_zoom_;
    const bool urgent = map_tiles_.empty() || ext_changed || raster_scale < 0.80f ||
                        raster_scale > 1.25f ||
                        std::abs(raster_off.x) > map_raster_overscan_ * 0.55f ||
                        std::abs(raster_off.y) > map_raster_overscan_ * 0.55f;
    if (urgent || (moved && map_raster_cooldown_ == 0)) {
        rebuild_map_raster(panel, ppm);
        map_raster_ext_ = UVec2{ext.width, ext.height};
        map_raster_cooldown_ = 6; // ~0.1 s between non-urgent rebuilds
        raster_scale = 1.0f;
        raster_off = Vec2{0.0f};
    }
    const f32 clip_l = panel.x + kMapInset, clip_t = panel.y + kMapInset;
    renderer_->draw_ui_tiles(map_tiles_, mc, raster_off, raster_scale,
                             Vec4{clip_l, clip_t, panel.z - 2.0f * kMapInset, panel.w - 2.0f * kMapInset});

    auto to_screen = [&](f32 wx, f32 wz) {
        return Vec2{mc.x + (wx - map_center_.x) * ppm, mc.y + (wz - map_center_.y) * ppm};
    };
    auto in_panel = [&](const Vec2& p, f32 pad = 4.0f) {
        return p.x > panel.x + kMapInset + pad && p.x < panel.x + panel.z - kMapInset - pad &&
               p.y > panel.y + kMapInset + pad && p.y < panel.y + panel.w - kMapInset - pad;
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
            const f32 ns = glm::clamp(r * 0.7f, 11.0f, 15.0f);
            hud::text(draw, Vec2{c.x, c.y - r - ns - 5.0f}, name, ns,
                      is_dest ? hud::kGold : Vec4{0.98f, 0.94f, 0.84f, 1.0f}, ui::TextAlign::Center,
                      ui::FontFace::Display);
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

    // Players: each in their identity colour; the local player is ringed white with a facing
    // tick. Drawn even when panned away, so you can always see where everyone is.
    if (have_snapshot_) {
        for (const net::PlayerState& pl : snapshot_.players) {
            const Vec2 p = to_screen(pl.position.x, pl.position.z);
            if (!in_panel(p)) {
                continue;
            }
            const Vec3 col = player_color(pl.color);
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

    // The frame over the raster's edges: a gold binding, an inset hairline and corner studs.
    draw.outline(panel, th.panel_border, 2.5f, 14.0f);
    const Vec4 board{panel.x + kMapInset, panel.y + kMapInset, panel.z - 2.0f * kMapInset,
                     panel.w - 2.0f * kMapInset};
    draw.outline(board, Vec4{0.08f, 0.05f, 0.03f, 1.0f}, 2.0f, 6.0f);
    for (const Vec2& c : {Vec2{board.x, board.y}, Vec2{board.x + board.z, board.y},
                          Vec2{board.x + board.z, board.y + board.w}, Vec2{board.x, board.y + board.w}}) {
        hud::stud(draw, c, 4.5f);
    }
    // Title cartouche, top-left over the map.
    {
        const f32 ts = 26.0f;
        const f32 tw = hud::heading_width("WORLD MAP", ts);
        const Vec4 cart{board.x + 14.0f, board.y + 12.0f, tw + 44.0f, ts + 26.0f};
        hud::plaque(draw, cart, 0.9f);
        hud::heading(draw, Vec2{cart.x + 22.0f, cart.y + 13.0f}, "WORLD MAP", ts);
    }

    // Legend (bottom-left): map symbols, the road-difficulty key, and the biome palette.
    constexpr int nleg = 14;
    const f32 lx = board.x + 26.0f;
    f32 ly = board.y + board.w - 22.0f - static_cast<f32>(nleg) * 18.0f;
    hud::plaque(draw, Vec4{lx - 12.0f, ly - 12.0f, 176.0f, static_cast<f32>(nleg) * 18.0f + 18.0f}, 0.9f);
    auto legend = [&](const Vec4& sw, const char* label, bool ring) {
        draw.rect(Vec4{lx, ly, 12.0f, 12.0f}, sw, ring ? Vec4{1.0f, 1.0f, 1.0f, 1.0f} : hud::kInk,
                  ring ? 2.0f : 1.0f, 6.0f);
        hud::text(draw, Vec2{lx + 20.0f, ly + 0.5f}, label, 11.0f, th.text);
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

    // Controls, bottom-right, sized to their text so they never run off the frame.
    {
        const char* help = "[DRAG] PAN   [SCROLL] ZOOM   [M] / [ESC] CLOSE";
        const f32 hs = 12.0f;
        const f32 hw = hud::rich_width(help, hs);
        const Vec4 hp{board.x + board.z - hw - 40.0f, board.y + board.w - hs * 2.6f - 14.0f, hw + 26.0f,
                      hs * 2.6f};
        hud::plaque(draw, hp, 0.9f);
        hud::rich(draw, Vec2{hp.x + 13.0f, hp.y + (hp.w - hs) * 0.5f}, help, hs, th.text);
    }
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

    const f32 pw = std::min(W * 0.62f, 720.0f);
    const f32 ph = std::min(H * 0.86f, 620.0f);
    const Vec4 panel{(W - pw) * 0.5f, (H - ph) * 0.5f, pw, ph};
    f32 y = hud::window(draw, W, H, panel, "WARDROBE");
    const f32 x = panel.x + 34.0f;
    // The party purse on the title row.
    {
        const std::string money = std::format("{}", snapshot_.money);
        const f32 ms = 22.0f;
        const f32 mw = hud::width(money, ms);
        const f32 mx = panel.x + panel.z - 34.0f;
        hud::text(draw, Vec2{mx, panel.y + 30.0f}, money, ms, hud::kGold, ui::TextAlign::Right);
        hud::coin(draw, Vec2{mx - mw - 18.0f, panel.y + 30.0f + ms * 0.5f}, ms * 0.48f);
    }
    // A wardrobe button: a gold plaque when it can be bought, a dimmed board otherwise.
    const Vec2 mouse = pointer_pos();
    auto shop_button = [&](const Vec4& btn, const std::string& label, u32 price, bool can) {
        const bool hot = can && in_rect(mouse, ui::Rect{btn.x, btn.y, btn.z, btn.w});
        if (can) {
            draw.shadow(btn, 8.0f, hot ? 12.0f : 6.0f, hud::alpha(th.accent_hover, hot ? 0.45f : 0.25f));
        }
        draw.shadow(btn, 8.0f, 5.0f, Vec4{0.0f, 0.0f, 0.0f, 0.5f}, Vec2{0.0f, 3.0f});
        draw.gradient(btn, can ? hud::shade(th.accent_hover, hot ? 1.08f : 1.0f) : Vec4{0.2f, 0.15f, 0.1f, 0.95f},
                      can ? hud::shade(th.accent, 0.72f) : Vec4{0.12f, 0.09f, 0.06f, 0.95f}, 8.0f,
                      can ? hud::shade(th.accent, 0.5f) : hud::alpha(th.accent, 0.35f), 1.5f);
        ui::TextStyle s = hud::style(16.0f, can ? th.accent_text : th.text_muted);
        s.outline = Vec4{0.0f};
        s.shadow = can ? Vec4{1.0f, 0.9f, 0.6f, 0.35f} : Vec4{0.0f, 0.0f, 0.0f, 0.5f};
        draw.text(Vec2{btn.x + 16.0f, btn.y + (btn.w - 16.0f) * 0.5f}, label, s);
        const std::string p = std::format("{}", price);
        s.align = ui::TextAlign::Right;
        draw.text(Vec2{btn.x + btn.z - 16.0f, btn.y + (btn.w - 16.0f) * 0.5f}, p, s);
        hud::coin(draw, Vec2{btn.x + btn.z - 16.0f - ui::DrawList::text_width(p, s) - 14.0f, btn.y + btn.w * 0.5f},
                  8.0f);
    };

    const net::PlayerState* me = local_player();
    const u8 owned = me != nullptr ? me->owned_tier : 0;
    const Vec3 feet = local_feet();
    const bool in_town =
        world_seed_ != 0 && worldgen::inside_village(feet.x, feet.z, world_seed_, 6.0f);

    // A section caption: small tracked gold capitals on a hairline rule.
    const f32 right = panel.x + panel.z - 34.0f;
    auto section = [&](const char* label) {
        const f32 lw = hud::text(draw, Vec2{x, y}, label, 12.0f, hud::alpha(th.accent_hover, 0.9f),
                                 ui::TextAlign::Left, ui::FontFace::Display);
        draw.line(Vec2{x + lw + 12.0f, y + 6.0f}, Vec2{right, y + 6.0f}, 1.0f, hud::alpha(th.accent, 0.35f));
        y += 24.0f;
    };

    // Current kit + its stat bonus.
    section("YOUR KIT");
    hud::text(draw, Vec2{x, y},
              std::format("{}  -  {} KIT", role_name(role_), tier_name(static_cast<EquipmentTier>(owned))),
              20.0f, Vec4{glm::mix(accent, Vec3{1.0f}, 0.35f), 1.0f}, ui::TextAlign::Left, ui::FontFace::Display);
    y += 32.0f;
    Equipment cur;
    cur.outfit_tier = owned;
    cur.weapon_tier = owned;
    const EquipBonus eb = equipment_bonus(cur);
    {
        f32 cx = x;
        const f32 cs = 12.0f;
        cx += hud::chip(draw, Vec2{cx, y}, std::format("+{} HP", static_cast<int>(eb.health_add)), cs, hud::kGood) + 8.0f;
        cx += hud::chip(draw, Vec2{cx, y}, std::format("X{:.2f} DAMAGE", eb.damage_mult), cs, hud::kWarn) + 8.0f;
        hud::chip(draw, Vec2{cx, y}, std::format("+{:.0f}% ARMOUR", eb.mitigation_add * 100.0f), cs,
                  Vec4{0.7f, 0.82f, 0.98f, 1.0f});
    }
    y += 42.0f;

    // Buy-upgrade button (server gates it on being in a town + affordability; this just requests it).
    section("SHOP");
    wardrobe_buy_rect_ = ui::Rect{};
    if (static_cast<u8>(owned + 1) < kTierCount) {
        const auto next = static_cast<EquipmentTier>(owned + 1);
        const u32 price = tier_price(next);
        const bool affordable = snapshot_.money >= price;
        const bool can = in_town && affordable;
        const Vec4 btn{x, y, std::min(400.0f, right - x), 46.0f};
        wardrobe_buy_rect_ = ui::Rect{btn.x, btn.y, btn.z, btn.w};
        shop_button(btn, std::format("BUY {} KIT", tier_name(next)), price, can);
        y += 54.0f;
        const char* hint = !in_town       ? "VISIT A TOWN SHOP TO BUY UPGRADES"
                           : !affordable  ? "NOT ENOUGH GOLD - DELIVER MORE CARGO"
                                          : "NICER LOOK + MORE HEALTH, DAMAGE & ARMOUR";
        draw.text(Vec2{x + 4.0f, y}, hint, 13.0f, can ? th.text_muted : hud::alpha(hud::kWarn, 0.9f));
        y += 30.0f;
    } else {
        hud::text(draw, Vec2{x, y}, "FULLY UPGRADED  -  MASTER GEAR", 17.0f, hud::kGold);
        y += 40.0f;
    }

    // Wagon-RIG upgrade (a money sink): reinforce the cart for more max health + ambush-damage resist.
    if (const u8 rl = snapshot_.rig_level; rl < kMaxRigLevel) {
        const u32 rprice = rig_price(static_cast<u8>(rl + 1));
        const bool rcan = in_town && snapshot_.money >= rprice;
        const Vec4 btn{x, y, std::min(400.0f, right - x), 42.0f};
        wardrobe_rig_rect_ = ui::Rect{btn.x, btn.y, btn.z, btn.w};
        shop_button(btn, std::format("REINFORCE WAGON  LV {}", rl + 1), rprice, rcan);
        y += 58.0f;
    } else {
        wardrobe_rig_rect_ = ui::Rect{};
        hud::text(draw, Vec2{x, y}, "WAGON FULLY REINFORCED", 16.0f, hud::kGold);
        y += 34.0f;
    }

    // Recolour swatches (the player's chosen primary colour), as glazed tiles.
    section("OUTFIT COLOUR");
    const f32 sw = 42.0f;
    for (int i = 0; i < 8; ++i) {
        const Vec4 r{x + static_cast<f32>(i) * (sw + 10.0f), y, sw, sw};
        wardrobe_swatch_rects_[i] = ui::Rect{r.x, r.y, r.z, r.w};
        const Vec3 c = outfit_tint_of(static_cast<u8>(i));
        const bool sel = equip_loadout_.outfit_tint == static_cast<u8>(i);
        if (sel) {
            draw.shadow(r, 7.0f, 8.0f, hud::alpha(th.accent_hover, 0.55f));
            draw.outline(Vec4{r.x - 3.0f, r.y - 3.0f, r.z + 6.0f, r.w + 6.0f}, th.accent_hover, 2.5f, 9.0f);
        } else {
            draw.shadow(r, 7.0f, 3.0f, Vec4{0.0f, 0.0f, 0.0f, 0.5f}, Vec2{0.0f, 1.5f});
        }
        draw.gradient(r, Vec4{glm::mix(c, Vec3{1.0f}, 0.18f), 1.0f}, Vec4{c * 0.78f, 1.0f}, 7.0f,
                      Vec4{0.0f, 0.0f, 0.0f, 0.5f}, 1.0f);
        draw.line(Vec2{r.x + 6.0f, r.y + 3.0f}, Vec2{r.x + r.z - 6.0f, r.y + 3.0f}, 1.5f,
                  Vec4{1.0f, 1.0f, 1.0f, 0.22f});
    }
    y += sw + 26.0f;

    // Change weapon (cycles the role's options).
    section("WEAPON");
    const WeaponType wt = role_weapon(static_cast<u8>(role_), equip_loadout_.weapon_index);
    hud::text(draw, Vec2{x, y + 6.0f}, weapon_name(wt), 18.0f, Vec4{glm::mix(accent, Vec3{1.0f}, 0.35f), 1.0f},
              ui::TextAlign::Left, ui::FontFace::Display);
    wardrobe_weapon_rect_ = ui::Rect{};
    if (role_weapon_count(static_cast<u8>(role_)) > 1) {
        const Vec4 btn{x + 240.0f, y - 2.0f, 140.0f, 34.0f};
        wardrobe_weapon_rect_ = ui::Rect{btn.x, btn.y, btn.z, btn.w};
        const bool hot = in_rect(mouse, wardrobe_weapon_rect_);
        draw.shadow(btn, 8.0f, 5.0f, Vec4{0.0f, 0.0f, 0.0f, 0.5f}, Vec2{0.0f, 3.0f});
        draw.gradient(btn, hud::shade(th.button_hover, hot ? 1.4f : 1.15f), th.button, 8.0f,
                      hud::alpha(th.accent, hot ? 0.9f : 0.5f), 1.5f);
        hud::text(draw, Vec2{btn.x + btn.z * 0.5f, btn.y + (btn.w - 14.0f) * 0.5f}, "CHANGE", 14.0f, th.text,
                  ui::TextAlign::Center);
    }

    hud::rich(draw, Vec2{x, panel.y + panel.w - 36.0f}, "[U] / [ESC] CLOSE", 12.0f, th.text_muted);
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
