// ClientApp - side quests (Game/SideQuest.h): the town notice board's panel, the quest's waypoint +
// triumph banner, and its place in the world - a bandit camp, the treasure X and its chest, glowing
// moonpetals, and a beacon of light over the site so the party can find it from afar.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"
#include "HudStyle.h"

namespace alryn::game {

namespace {
// Each kind's colour: the beacon over its site, its map pin, its HUD accents.
Vec3 quest_color(QuestKind k) {
    switch (k) {
        case QuestKind::BanditCamp: return Vec3{1.0f, 0.45f, 0.22f}; // campfire red
        case QuestKind::WolfHunt: return Vec3{1.0f, 0.78f, 0.3f};    // amber, the pack's eyes
        case QuestKind::Treasure: return Vec3{1.0f, 0.86f, 0.36f};   // gold
        case QuestKind::Herbs: return Vec3{0.55f, 0.82f, 1.0f};      // moonlight
    }
    return Vec3{1.0f};
}
} // namespace

const net::QuestState* ClientApp::active_quest() const {
    if (!have_snapshot_) {
        return nullptr;
    }
    for (const net::QuestState& q : snapshot_.quests) {
        if (q.phase == static_cast<u8>(QuestPhase::Active)) {
            return &q;
        }
    }
    return nullptr;
}

bool ClientApp::near_quest_board() const {
    if (!have_snapshot_) {
        return false;
    }
    const Vec3 feet = local_feet();
    for (const net::QuestState& q : snapshot_.quests) {
        if (q.phase != static_cast<u8>(QuestPhase::Complete) &&
            glm::length(Vec2{q.board.x - feet.x, q.board.z - feet.z}) < kQuestBoardRange) {
            return true;
        }
    }
    return false;
}

bool ClientApp::quest_panel_click(const Vec2& p) {
    for (u8 i = 0; i < kQuestOffers; ++i) {
        if (quest_accept_ids_[i] != 0 && in_rect(p, quest_accept_rects_[i])) {
            pending_quest_pick_ = quest_accept_ids_[i];
            quest_pick_hold_ = 8; // held a few ticks so the server sees it even if a packet drops
            if (Audio* a = audio()) {
                a->play(SfxId::UiClick);
            }
            return true;
        }
    }
    if (in_rect(p, quest_abandon_rect_)) {
        quest_abandon_hold_ = 6;
        if (Audio* a = audio()) {
            a->play(SfxId::UiClick, 0.8f, 0.8f);
        }
        return true;
    }
    return false;
}

void ClientApp::update_quest_fx(Timestep dt) {
    quest_banner_ = std::max(0.0f, quest_banner_ - dt.seconds);
    if (!have_snapshot_) {
        return;
    }
    Audio* a = audio();
    for (const net::QuestState& q : snapshot_.quests) {
        // A quest just done: a fanfare, a banner naming it and the pay, a shower of gold.
        if (q.phase == static_cast<u8>(QuestPhase::Complete) && q.id != last_quest_done_) {
            last_quest_done_ = q.id;
            quest_banner_ = 5.0f;
            quest_banner_text_ = std::format("{}  -  COMPLETE", quest_title(static_cast<QuestKind>(q.kind)));
            if (a != nullptr) {
                a->play(SfxId::Fanfare);
            }
            const Vec3 at = local_feet() + Vec3{0.0f, 1.2f, 0.0f};
            emit_burst(at, Vec4{1.0f, 0.86f, 0.4f, 1.0f}, 40, 4.0f, 1.0f, 0.12f, 1, 4.0f, 4.0f);
        }
    }
    const net::QuestState* q = active_quest();
    if (q == nullptr) {
        last_quest_id_ = 0;
        return;
    }
    if (q->id != last_quest_id_) {
        // Freshly taken: a horn sounds the party out.
        last_quest_id_ = q->id;
        last_quest_progress_ = q->progress;
        if (a != nullptr) {
            a->play(SfxId::Horn, 0.75f);
        }
        combat_text(local_feet(), "QUEST ACCEPTED", Vec4{0.7f, 0.9f, 1.0f, 1.0f}, 24.0f);
        return;
    }
    if (q->progress > last_quest_progress_) {
        // A step closer: "+1" floats up (a petal picked, a spade strike that rang true, a foe down).
        combat_text(local_feet(), std::format("{} / {}", q->progress, q->goal), Vec4{0.72f, 0.92f, 1.0f, 1.0f}, 20.0f);
        if (a != nullptr && q->kind != static_cast<u8>(QuestKind::BanditCamp) && q->kind != static_cast<u8>(QuestKind::WolfHunt)) {
            a->play(SfxId::Coin, 0.55f, 1.25f);
        }
        if (q->kind == static_cast<u8>(QuestKind::Treasure) && q->progress >= q->goal) {
            combat_text(q->site, "THE SPADE STRIKES WOOD!", Vec4{1.0f, 0.86f, 0.4f, 1.0f}, 24.0f);
        }
    }
    last_quest_progress_ = q->progress;
}

void ClientApp::draw_quest_panel(ui::DrawList& draw, f32 W, f32 H, f32 ts) {
    const ui::Theme& th = ui::theme();
    const net::QuestState* active = active_quest();
    std::vector<const net::QuestState*> offers;
    for (const net::QuestState& q : snapshot_.quests) {
        if (q.phase == static_cast<u8>(QuestPhase::Offered)) {
            offers.push_back(&q);
        }
    }
    const f32 pw = glm::clamp(W * 0.3f, 330.0f, 470.0f);
    const f32 row_h = ts * 4.7f;
    const f32 ph = ts * 3.6f + (active != nullptr ? ts * 5.2f : row_h * static_cast<f32>(std::max<usize>(offers.size(), 1)));
    const f32 px = W - pw - 20.0f;
    const f32 py = glm::clamp(H * 0.22f, 80.0f, H - ph - 140.0f);
    const Vec4 card{px, py, pw, ph};
    draw.shadow(card, 12.0f, 18.0f, Vec4{0.0f, 0.0f, 0.0f, 0.6f}, Vec2{0.0f, 6.0f});
    draw.gradient(card, th.panel, th.panel_bottom, 12.0f, th.panel_border, 1.75f);
    draw.outline(Vec4{px + 5.0f, py + 5.0f, pw - 10.0f, ph - 10.0f}, hud::alpha(th.accent, 0.3f), 1.0f, 8.0f);
    const f32 ix = px + 20.0f;
    const f32 right = px + pw - 20.0f;
    f32 iy = py + 16.0f;
    hud::text(draw, Vec2{ix, iy}, "WORK BESIDES THE WAGONS", ts * 0.54f, th.text_muted);
    iy += ts * 0.95f;
    hud::heading(draw, Vec2{ix, iy}, "NOTICE BOARD", ts * 1.05f);
    iy += ts * 1.6f;

    const Vec2 mouse = pointer_pos();
    auto button = [&](const ui::Rect& r, const char* label, Vec3 base, Vec4 text_col) {
        const bool hot = in_rect(mouse, r);
        const Vec4 rr{r.x, r.y, r.w, r.h};
        draw.shadow(rr, 6.0f, 5.0f, Vec4{0.0f, 0.0f, 0.0f, 0.55f}, Vec2{0.0f, 2.5f});
        draw.gradient(rr, Vec4{base * (hot ? 1.45f : 1.25f), 1.0f}, Vec4{base * (hot ? 0.95f : 0.8f), 1.0f}, 6.0f,
                      hud::alpha(th.accent_hover, hot ? 0.95f : 0.6f), 1.5f);
        hud::text(draw, Vec2{rr.x + rr.z * 0.5f, rr.y + (rr.w - ts * 0.62f) * 0.5f}, label, ts * 0.62f, text_col,
                  ui::TextAlign::Center);
    };
    auto danger_pips = [&](f32 x, f32 y, u8 danger) {
        const Vec4 dcol = danger <= 1 ? hud::kGood : danger == 2 ? hud::kWarn : hud::kBad;
        for (int k = 0; k < 3; ++k) {
            const f32 pr = ts * 0.18f;
            const Vec2 pc{x + static_cast<f32>(k) * pr * 2.6f + pr, y};
            draw.rect(Vec4{pc.x - pr, pc.y - pr, pr * 2.0f, pr * 2.0f}, k < danger ? dcol : Vec4{0.2f, 0.15f, 0.1f, 0.9f}, pr);
            draw.outline(Vec4{pc.x - pr, pc.y - pr, pr * 2.0f, pr * 2.0f}, hud::kInk, 1.0f, pr);
        }
    };

    if (active != nullptr) {
        const auto kind = static_cast<QuestKind>(active->kind);
        hud::text(draw, Vec2{ix, iy}, "UNDER WAY", ts * 0.54f, Vec4{quest_color(kind), 1.0f});
        iy += ts * 0.9f;
        hud::text(draw, Vec2{ix, iy}, quest_title(kind), ts * 0.82f, hud::kGold, ui::TextAlign::Left, ui::FontFace::Display);
        iy += ts * 1.25f;
        hud::rich(draw, Vec2{ix, iy}, std::format("{}   {} / {}", quest_objective(kind), active->progress, active->goal),
                  ts * 0.56f, th.text);
        iy += ts * 1.3f;
        quest_abandon_rect_ = ui::Rect{ix, py + ph - ts * 1.6f - 14.0f, pw - 40.0f, ts * 1.6f};
        button(quest_abandon_rect_, "ABANDON", Vec3{0.30f, 0.12f, 0.09f}, Vec4{1.0f, 0.86f, 0.8f, 1.0f});
        return;
    }
    if (offers.empty()) {
        hud::text(draw, Vec2{ix, iy}, "NOTHING POSTED - CHECK BACK LATER", ts * 0.58f, th.text_muted);
        return;
    }
    for (usize i = 0; i < offers.size() && i < kQuestOffers; ++i) {
        const net::QuestState& q = *offers[i];
        const auto kind = static_cast<QuestKind>(q.kind);
        draw.line(Vec2{ix, iy - ts * 0.25f}, Vec2{right, iy - ts * 0.25f}, 1.0f, hud::alpha(th.accent, 0.35f));
        // The kind's colour stud, the title, its danger and pay.
        draw.rect(Vec4{ix, iy + ts * 0.12f, ts * 0.42f, ts * 0.42f}, Vec4{quest_color(kind), 1.0f}, ts * 0.21f);
        hud::text(draw, Vec2{ix + ts * 0.65f, iy}, quest_title(kind), ts * 0.66f, hud::kGold, ui::TextAlign::Left,
                  ui::FontFace::Display);
        danger_pips(right - ts * 1.6f, iy + ts * 0.32f, q.danger);
        iy += ts * 1.05f;
        const std::vector<std::string> lines = hud::wrap(quest_blurb(kind), ts * 0.5f, pw - 40.0f);
        for (const std::string& line : lines) {
            hud::text(draw, Vec2{ix, iy}, line, ts * 0.5f, th.text);
            iy += ts * 0.68f;
        }
        const f32 dist = glm::length(Vec2{q.site.x - local_feet().x, q.site.z - local_feet().z});
        const std::string pay = std::format("{}", q.reward);
        hud::coin(draw, Vec2{ix + ts * 0.3f, iy + ts * 0.42f}, ts * 0.3f);
        hud::text(draw, Vec2{ix + ts * 0.75f, iy + ts * 0.12f}, std::format("{}   -   ~{} M OUT", pay, static_cast<int>(dist)),
                  ts * 0.56f, hud::kGold);
        const f32 bw = ts * 5.2f;
        quest_accept_rects_[i] = ui::Rect{right - bw, iy - ts * 0.05f, bw, ts * 1.35f};
        quest_accept_ids_[i] = q.id;
        button(quest_accept_rects_[i], "ACCEPT", Vec3{0.20f, 0.32f, 0.13f}, Vec4{0.9f, 1.0f, 0.82f, 1.0f});
        iy = py + ts * 3.6f + row_h * static_cast<f32>(i + 1);
    }
}

void ClientApp::draw_quest_hud(ui::DrawList& draw, f32 W, f32 H, f32 ts) {
    for (u8 i = 0; i < kQuestOffers; ++i) {
        quest_accept_rects_[i] = ui::Rect{};
        quest_accept_ids_[i] = 0;
    }
    quest_abandon_rect_ = ui::Rect{};
    if (!have_snapshot_) {
        return;
    }
    const Vec3 feet = local_feet();
    const net::QuestState* active = active_quest();
    // A gold "!" over a notice board with work pinned to it (or the one our quest came from), so the
    // boards are found without hunting.
    for (const net::QuestState& q : snapshot_.quests) {
        if (q.phase == static_cast<u8>(QuestPhase::Complete) || (active != nullptr && &q != active)) {
            continue;
        }
        const f32 d = glm::length(Vec2{q.board.x - feet.x, q.board.z - feet.z});
        if (d > 60.0f || d < kQuestBoardRange) {
            continue;
        }
        Vec2 sp;
        const f32 bob = 0.12f * std::sin(elapsed_ * 3.0f);
        if (world_to_screen(q.board + Vec3{0.0f, 2.9f + bob, 0.0f}, W, H, sp)) {
            const f32 r = ts * 0.62f;
            draw.glow(Vec4{sp.x - r * 2.0f, sp.y - r * 2.0f, r * 4.0f, r * 4.0f}, hud::alpha(hud::kGold, 0.35f),
                      Vec4{1.0f, 0.85f, 0.45f, 0.0f});
            hud::medallion(draw, sp, r, Vec4{0.08f, 0.05f, 0.03f, 1.0f});
            hud::text(draw, Vec2{sp.x, sp.y - ts * 0.42f}, active != nullptr ? "?" : "!", ts * 0.85f, hud::kGold,
                      ui::TextAlign::Center, ui::FontFace::Display);
            hud::text(draw, Vec2{sp.x, sp.y + r + 4.0f}, "QUESTS", ts * 0.46f, hud::kGold, ui::TextAlign::Center);
        }
        break; // one board per town
    }
    if (near_quest_board()) {
        draw_quest_panel(draw, W, H, ts);
    }
    // The quest site: a marker hanging over it (an edge pointer when it's off-screen) with the distance.
    if (active != nullptr) {
        const auto kind = static_cast<QuestKind>(active->kind);
        const Vec4 col{quest_color(kind), 1.0f};
        const f32 d = glm::length(Vec2{active->site.x - feet.x, active->site.z - feet.z});
        Vec2 sp;
        const bool on = world_to_screen(active->site + Vec3{0.0f, 4.0f + 0.25f * std::sin(elapsed_ * 2.4f), 0.0f}, W, H, sp) &&
                        sp.x > 30.0f && sp.x < W - 30.0f && sp.y > 30.0f && sp.y < H - 30.0f;
        if (on && d > 6.0f) {
            const f32 r = ts * 0.5f;
            draw.glow(Vec4{sp.x - r * 2.2f, sp.y - r * 2.2f, r * 4.4f, r * 4.4f}, hud::alpha(col, 0.35f), Vec4{Vec3{col}, 0.0f});
            draw.line(Vec2{sp.x, sp.y - r}, Vec2{sp.x + r, sp.y}, 3.0f, col);
            draw.line(Vec2{sp.x + r, sp.y}, Vec2{sp.x, sp.y + r}, 3.0f, col);
            draw.line(Vec2{sp.x, sp.y + r}, Vec2{sp.x - r, sp.y}, 3.0f, col);
            draw.line(Vec2{sp.x - r, sp.y}, Vec2{sp.x, sp.y - r}, 3.0f, col);
            hud::text(draw, Vec2{sp.x, sp.y + r + 5.0f}, std::format("{} M", static_cast<int>(d)), ts * 0.5f, col,
                      ui::TextAlign::Center);
        } else if (!on) {
            // Off-screen: a pointer pinned to the screen edge in the site's direction.
            Vec2 dir{0.0f, -1.0f};
            const Vec4 clip = camera_.view_projection() * Vec4{active->site, 1.0f};
            if (std::abs(clip.w) > 1e-3f) {
                Vec2 ndc{clip.x / clip.w, clip.y / clip.w};
                if (clip.w < 0.0f) {
                    ndc = -ndc;
                }
                if (glm::length(ndc) > 1e-3f) {
                    dir = glm::normalize(ndc);
                }
            }
            const Vec2 c{W * 0.5f, H * 0.5f};
            const f32 k = std::min((W * 0.5f - 46.0f) / std::max(std::abs(dir.x), 1e-3f),
                                   (H * 0.5f - 46.0f) / std::max(std::abs(dir.y), 1e-3f));
            const Vec2 p = c + dir * k;
            const Vec2 side{-dir.y, dir.x};
            const f32 s = ts * 0.55f;
            draw.line(p + dir * s, p - dir * s * 0.6f + side * s * 0.8f, 3.5f, col);
            draw.line(p + dir * s, p - dir * s * 0.6f - side * s * 0.8f, 3.5f, col);
            hud::text(draw, p - dir * s * 1.8f - Vec2{0.0f, s * 0.5f}, std::format("{} M", static_cast<int>(d)), ts * 0.48f, col,
                      ui::TextAlign::Center);
        }
    }
    // E prompts beside a pickup in reach: crack the unearthed chest, pick a moonpetal.
    for (const net::QuestItemState& it : snapshot_.quest_items) {
        const bool chest = it.kind == 1u && it.state == 1u;
        const bool petal = it.kind == 0u && it.state == 0u;
        const f32 reach = chest ? kChestOpenRange : kHerbPickRange;
        if ((!chest && !petal) || glm::length(Vec2{it.position.x - feet.x, it.position.z - feet.z}) > reach + 0.4f) {
            continue;
        }
        Vec2 sp;
        if (world_to_screen(it.position + Vec3{0.0f, 1.3f, 0.0f}, W, H, sp)) {
            const char* hint = chest ? "[E] OPEN THE CHEST" : "[E] PICK THE MOONPETAL";
            hud::rich(draw, Vec2{sp.x - hud::rich_width(hint, ts * 0.6f) * 0.5f, sp.y}, hint, ts * 0.6f, hud::kGold);
        }
        break;
    }
    if (active != nullptr && active->kind == static_cast<u8>(QuestKind::Treasure) &&
        glm::length(Vec2{active->site.x - feet.x, active->site.z - feet.z}) < 9.0f &&
        std::none_of(snapshot_.quest_items.begin(), snapshot_.quest_items.end(),
                     [](const net::QuestItemState& it) { return it.kind == 1u && it.state != 0u; })) {
        Vec2 sp;
        if (world_to_screen(active->site + Vec3{0.0f, 1.0f, 0.0f}, W, H, sp)) {
            const char* hint = "AIM AT THE X  -  [Q] DIG";
            hud::rich(draw, Vec2{sp.x - hud::rich_width(hint, ts * 0.6f) * 0.5f, sp.y}, hint, ts * 0.6f, hud::kGold);
        }
    }
    if (quest_banner_ > 0.0f) {
        const f32 bs = glm::clamp(H * 0.045f, 22.0f, 52.0f);
        hud::banner(draw, W * 0.5f, H * 0.36f, quest_banner_text_, bs, hud::kGood);
    }
}

void ClientApp::dev_quest_setup() {
    if (!have_snapshot_ || dev_setup_wait_ < 1.0f) {
        return;
    }
    if (const char* h = std::getenv("ALRYN_HOLD"); h != nullptr && h[0] == '1' && !attack_held_) {
        attack_press(); // held for the rest of the run: the wind-up builds to a full charge
    }
    if (const char* h = std::getenv("ALRYN_HOLD"); h != nullptr && h[0] == '2') {
        // Hold to a full charge, unleash it, repeat (the heavy's release + impact + craters).
        const f32 cycle = std::fmod(dev_setup_wait_ - 1.0f, 3.2f);
        if (cycle < 1.6f && !attack_held_) {
            attack_press();
        } else if (cycle >= 1.6f && attack_held_) {
            attack_release();
        }
    }
    const std::string_view scr = dev_screen();
    if (dev_setup_done_ || (scr != "quest" && scr != "board" && scr != "bestiary") || !host_local_ ||
        !local_server_.running()) {
        dev_setup_done_ = true;
        return;
    }
    std::lock_guard<std::mutex> lock(server_mutex_);
    if (scr == "bestiary") {
        // Every foe in a row on the plaza ahead (godmode on, so the hero stands among them).
        debug_god_ = true;
        local_server_.set_debug_god(true);
        std::vector<u8> kinds; // ALRYN_BESTIARY=8,9 limits it to those kinds
        if (const char* b = std::getenv("ALRYN_BESTIARY"); b != nullptr) {
            for (const char* c = b; *c != '\0';) {
                kinds.push_back(static_cast<u8>(std::atoi(c)));
                while (*c != '\0' && *c != ',') {
                    ++c;
                }
                if (*c == ',') {
                    ++c;
                }
            }
        }
        // ALRYN_BESTIARY_FREEZE=1: they stand still between us and the camera, faces to it (close-ups).
        const char* fz = std::getenv("ALRYN_BESTIARY_FREEZE");
        const bool frozen = fz != nullptr && fz[0] == '1';
        const Vec3 at = local_feet();
        const f32 yaw = radians(iso::yaw_deg + (frozen ? 0.0f : 180.0f));
        local_server_.debug_spawn_bestiary(at, yaw, kinds, frozen);
        if (frozen) {
            // Step up just behind the line, so the camera frames it with us at its back.
            const Vec3 p = at + Vec3{std::cos(yaw), 0.0f, std::sin(yaw)} * 1.2f;
            local_server_.debug_place_player(my_id_, Vec3{p.x, worldgen::height(p.x, p.z, world_seed_) + 0.5f, p.z});
        }
        dev_setup_done_ = true;
        return;
    }
    const auto& qs = local_server_.quests();
    if (qs.empty()) {
        return; // the board hasn't been pinned yet
    }
    if (scr == "board") {
        const Vec3 b = qs.front().board;
        local_server_.debug_place_player(my_id_, b + Vec3{1.2f, 0.6f, 1.2f});
        dev_setup_done_ = true;
        return;
    }
    int want = -1;
    if (const char* k = std::getenv("ALRYN_QUEST_KIND"); k != nullptr) {
        want = std::atoi(k);
    }
    for (const GameServer::QuestRun& q : qs) {
        if (q.phase == QuestPhase::Offered && (want < 0 || static_cast<int>(q.kind) == want || dev_setup_wait_ > 4.0f)) {
            const Vec3 site = q.site;
            const u32 id = q.id;
            local_server_.debug_accept_quest(id);
            // Stand back from the site toward the town (close enough to wake it, far enough to see it).
            Vec2 back{q.board.x - site.x, q.board.z - site.z};
            back = glm::length(back) > 1e-3f ? glm::normalize(back) : Vec2{1.0f, 0.0f};
            const f32 stand = static_cast<int>(q.kind) <= 1 ? 9.0f : 5.0f;
            const Vec2 p = Vec2{site.x, site.z} + back * stand;
            local_server_.debug_place_player(my_id_, Vec3{p.x, worldgen::height(p.x, p.y, world_seed_) + 0.6f, p.y});
            face_yaw_ = std::atan2(-back.y, -back.x);
            dev_setup_done_ = true;
            return;
        }
    }
}

void ClientApp::draw_quest_world() {
    const net::QuestState* q = active_quest();
    if (q == nullptr || renderer_ == nullptr || terrain_ == nullptr) {
        return;
    }
    const auto kind = static_cast<QuestKind>(q->kind);
    const Vec3 col = quest_color(kind);
    const Vec3 feet = local_feet();
    const f32 dist = glm::length(Vec2{q->site.x - feet.x, q->site.z - feet.z});
    // The ground under (x,z) as the world stands now (digs + craters included).
    auto ground = [&](f32 x, f32 z) {
        if (const auto g = terrain_->raycast(Vec3{x, 60.0f, z}, Vec3{0.0f, -1.0f, 0.0f}, 120.0f)) {
            return g->y;
        }
        return worldgen::height(x, z, world_seed_);
    };
    auto hash01 = [&](u32 salt) {
        u32 h = q->id * 2654435761u ^ (salt * 40503u);
        h ^= h >> 15;
        h *= 0x2c1b3c6du;
        h ^= h >> 12;
        return static_cast<f32>(h & 0xFFFFu) / 65535.0f;
    };
    // A shaft of light standing over the site, seen from far across the land (it fades as you arrive).
    if (dist > 14.0f) {
        const f32 a = glm::smoothstep(14.0f, 40.0f, dist) * (0.22f + 0.06f * std::sin(elapsed_ * 2.0f));
        const Vec3 base{q->site.x, ground(q->site.x, q->site.z), q->site.z};
        renderer_->draw_sprite(base, base + Vec3{0.0f, 34.0f, 0.0f}, 0.55f, Vec4{col, a}, 0.15f);
        renderer_->draw_sprite(base, base + Vec3{0.0f, 20.0f, 0.0f}, 0.18f, Vec4{glm::mix(col, Vec3{1.0f}, 0.5f), a * 1.6f}, 0.5f);
    }
    if (dist > 110.0f) {
        return; // the camp / X / petals only need drawing once we're near
    }
    const f32 night = 1.0f - sun_intensity_;
    switch (kind) {
        case QuestKind::BanditCamp: {
            // Three ragged tents round a fire, a crude stake palisade and the warband's banner.
            for (int i = 0; i < 3; ++i) {
                const f32 a = TwoPi * static_cast<f32>(i) / 3.0f + hash01(static_cast<u32>(i)) * 0.6f;
                const f32 r = 6.5f + 1.5f * hash01(static_cast<u32>(i) + 10u);
                const f32 x = q->site.x + std::cos(a) * r, z = q->site.z + std::sin(a) * r;
                renderer_->draw(tent_mesh_,
                                glm::translate(Mat4{1.0f}, Vec3{x, ground(x, z) - 0.05f, z}) *
                                    glm::rotate(Mat4{1.0f}, -a + HalfPi, Vec3{0.0f, 1.0f, 0.0f}));
            }
            for (int i = 0; i < 14; ++i) {
                const f32 a = TwoPi * static_cast<f32>(i) / 14.0f;
                if (i % 5 == 0) {
                    continue; // gaps in the stakes
                }
                const f32 r = 10.5f;
                const f32 x = q->site.x + std::cos(a) * r, z = q->site.z + std::sin(a) * r;
                const f32 h = 1.3f + 0.4f * hash01(static_cast<u32>(i) + 30u);
                renderer_->draw(shape_box_,
                                glm::translate(Mat4{1.0f}, Vec3{x, ground(x, z) + h * 0.45f, z}) *
                                    glm::rotate(Mat4{1.0f}, -a, Vec3{0.0f, 1.0f, 0.0f}) *
                                    glm::rotate(Mat4{1.0f}, 0.25f, Vec3{0.0f, 0.0f, 1.0f}) * glm::scale(Mat4{1.0f}, Vec3{0.14f, h, 0.14f}),
                                Vec4{0.34f, 0.24f, 0.14f, 1.0f});
            }
            // The fire: a ring of stones, logs, licking flames and a big warm light.
            const Vec3 fire{q->site.x, ground(q->site.x, q->site.z), q->site.z};
            for (int i = 0; i < 8; ++i) {
                const f32 a = TwoPi * static_cast<f32>(i) / 8.0f;
                renderer_->draw(shape_sphere_,
                                glm::translate(Mat4{1.0f}, fire + Vec3{std::cos(a) * 0.75f, 0.08f, std::sin(a) * 0.75f}) *
                                    glm::scale(Mat4{1.0f}, Vec3{0.3f, 0.2f, 0.28f}),
                                Vec4{0.36f, 0.34f, 0.32f, 1.0f});
            }
            for (int i = 0; i < 3; ++i) {
                renderer_->draw(shape_box_,
                                glm::translate(Mat4{1.0f}, fire + Vec3{0.0f, 0.12f, 0.0f}) *
                                    glm::rotate(Mat4{1.0f}, static_cast<f32>(i) * 1.05f, Vec3{0.0f, 1.0f, 0.0f}) *
                                    glm::scale(Mat4{1.0f}, Vec3{1.0f, 0.13f, 0.13f}),
                                Vec4{0.24f, 0.16f, 0.09f, 1.0f});
            }
            const f32 flick = 0.85f + 0.15f * std::sin(elapsed_ * 11.0f) + 0.05f * std::sin(elapsed_ * 23.0f);
            renderer_->draw_sprite(fire + Vec3{0.0f, 0.55f, 0.0f}, 0.75f * flick, Vec4{1.0f, 0.55f, 0.2f, 0.75f}, 0.3f);
            renderer_->draw_sprite(fire + Vec3{0.0f, 0.4f, 0.0f}, 0.35f, Vec4{1.0f, 0.88f, 0.55f, 1.0f}, 0.8f);
            if (frand() < 0.8f) {
                emit_ember(fire + Vec3{frand(-0.3f, 0.3f), 0.3f, frand(-0.3f, 0.3f)},
                           Vec3{frand(-0.3f, 0.3f), frand(1.6f, 3.0f), frand(-0.3f, 0.3f)}, Vec3{1.0f, 0.82f, 0.4f},
                           Vec3{0.7f, 0.14f, 0.04f}, frand(0.5f, 0.9f), frand(0.1f, 0.2f), -1.0f);
            }
            fx_light(fire + Vec3{0.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.55f, 0.22f}, (1.6f + 2.4f * night) * flick, 12.0f);
            // The warband's banner on a tall pole by the fire.
            const f32 bx = q->site.x + 2.2f, bz = q->site.z - 1.6f;
            const Vec3 pole{bx, ground(bx, bz), bz};
            renderer_->draw(shape_box_, glm::translate(Mat4{1.0f}, pole + Vec3{0.0f, 1.7f, 0.0f}) * glm::scale(Mat4{1.0f}, Vec3{0.08f, 3.4f, 0.08f}),
                            Vec4{0.2f, 0.14f, 0.09f, 1.0f});
            const f32 flap = 0.2f * std::sin(elapsed_ * 3.3f);
            renderer_->draw(shape_box_,
                            glm::translate(Mat4{1.0f}, pole + Vec3{0.45f, 2.9f, 0.0f}) * glm::rotate(Mat4{1.0f}, flap, Vec3{0.0f, 1.0f, 0.0f}) *
                                glm::scale(Mat4{1.0f}, Vec3{0.85f, 0.75f, 0.04f}),
                            Vec4{0.62f, 0.08f, 0.09f, 1.0f});
            renderer_->draw(shape_sphere_, glm::translate(Mat4{1.0f}, pole + Vec3{0.45f, 2.95f, 0.04f}) * glm::scale(Mat4{1.0f}, Vec3{0.22f}),
                            Vec4{0.84f, 0.8f, 0.68f, 1.0f}); // a skull sigil
            break;
        }
        case QuestKind::WolfHunt: {
            // The den: a heap of boulders, gnawed bones strewn about.
            for (int i = 0; i < 4; ++i) {
                const f32 a = hash01(static_cast<u32>(i) + 50u) * TwoPi;
                const f32 r = 2.0f + 2.5f * hash01(static_cast<u32>(i) + 51u);
                const f32 x = q->site.x + std::cos(a) * r, z = q->site.z + std::sin(a) * r;
                const f32 s = 0.9f + 0.9f * hash01(static_cast<u32>(i) + 52u);
                renderer_->draw(shape_sphere_,
                                glm::translate(Mat4{1.0f}, Vec3{x, ground(x, z) + s * 0.25f, z}) * glm::scale(Mat4{1.0f}, Vec3{s * 1.3f, s * 0.9f, s}),
                                Vec4{0.38f, 0.36f, 0.34f, 1.0f});
            }
            for (int i = 0; i < 9; ++i) {
                const f32 a = hash01(static_cast<u32>(i) + 60u) * TwoPi;
                const f32 r = 1.0f + 5.0f * hash01(static_cast<u32>(i) + 61u);
                const f32 x = q->site.x + std::cos(a) * r, z = q->site.z + std::sin(a) * r;
                renderer_->draw(shape_box_,
                                glm::translate(Mat4{1.0f}, Vec3{x, ground(x, z) + 0.04f, z}) *
                                    glm::rotate(Mat4{1.0f}, hash01(static_cast<u32>(i) + 62u) * TwoPi, Vec3{0.0f, 1.0f, 0.0f}) *
                                    glm::scale(Mat4{1.0f}, Vec3{0.42f, 0.06f, 0.06f}),
                                Vec4{0.86f, 0.82f, 0.72f, 1.0f});
            }
            break;
        }
        case QuestKind::Treasure: {
            const net::QuestItemState* chest = nullptr;
            for (const net::QuestItemState& it : snapshot_.quest_items) {
                if (it.kind == 1u) {
                    chest = &it;
                }
            }
            if (chest == nullptr || chest->state == 0u) {
                // The X marks the spot: two weathered red-daubed planks crossed on the ground, a cairn
                // beside it - dig here [Q].
                const Vec3 x{q->site.x, ground(q->site.x, q->site.z) + 0.04f, q->site.z};
                for (const f32 a : {0.785f, -0.785f}) {
                    renderer_->draw(shape_box_,
                                    glm::translate(Mat4{1.0f}, x) * glm::rotate(Mat4{1.0f}, a, Vec3{0.0f, 1.0f, 0.0f}) *
                                        glm::scale(Mat4{1.0f}, Vec3{1.8f, 0.05f, 0.24f}),
                                    Vec4{0.62f, 0.12f, 0.08f, 1.0f});
                }
                for (int i = 0; i < 3; ++i) {
                    renderer_->draw(shape_sphere_,
                                    glm::translate(Mat4{1.0f}, x + Vec3{1.7f, 0.18f + 0.26f * static_cast<f32>(i), 0.6f}) *
                                        glm::scale(Mat4{1.0f}, Vec3{0.42f - 0.1f * static_cast<f32>(i)}),
                                    Vec4{0.48f, 0.46f, 0.43f, 1.0f});
                }
                const f32 pulse = 0.5f + 0.5f * std::sin(elapsed_ * 3.0f);
                sprite_circle(x + Vec3{0.0f, 0.05f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, kTreasureDigReach, 32, 0.05f,
                              Vec4{col, 0.25f + 0.25f * pulse}, 0.5f);
            } else {
                // The chest, turned up in the hole: iron-bound, its lid thrown back once opened on a hoard
                // of gold that glitters and lights the pit.
                const Mat4 m = glm::translate(Mat4{1.0f}, chest->position) *
                               glm::rotate(Mat4{1.0f}, static_cast<f32>(q->id % 7u), Vec3{0.0f, 1.0f, 0.0f});
                renderer_->draw(shape_box_, m * glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.25f, 0.0f}) * glm::scale(Mat4{1.0f}, Vec3{0.95f, 0.5f, 0.6f}),
                                Vec4{0.42f, 0.27f, 0.13f, 1.0f});
                for (const f32 bx : {-0.32f, 0.32f}) {
                    renderer_->draw(shape_box_,
                                    m * glm::translate(Mat4{1.0f}, Vec3{bx, 0.27f, 0.0f}) * glm::scale(Mat4{1.0f}, Vec3{0.07f, 0.54f, 0.64f}),
                                    Vec4{0.22f, 0.21f, 0.22f, 1.0f});
                }
                const bool open = chest->state == 2u;
                const Mat4 lid = m * glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.5f, -0.3f}) *
                                 glm::rotate(Mat4{1.0f}, open ? -1.9f : 0.0f, Vec3{1.0f, 0.0f, 0.0f}) *
                                 glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.06f, 0.3f});
                renderer_->draw(shape_box_, lid * glm::scale(Mat4{1.0f}, Vec3{0.97f, 0.14f, 0.62f}), Vec4{0.46f, 0.3f, 0.15f, 1.0f});
                const Vec3 glint = Vec3{m * Vec4{0.0f, 0.62f, 0.0f, 1.0f}};
                renderer_->draw_sprite(glint, open ? 0.6f : 0.25f, Vec4{1.0f, 0.82f, 0.35f, open ? 0.7f : 0.35f}, 0.3f);
                if (open && frand() < 0.5f) {
                    emit(glint + rand_dir() * 0.25f, Vec3{0.0f, frand(0.5f, 1.4f), 0.0f}, Vec4{1.0f, 0.88f, 0.45f, 1.0f}, 0.7f, 0.06f, 1, -0.2f);
                }
                fx_light(glint + Vec3{0.0f, 0.5f, 0.0f}, Vec3{1.0f, 0.8f, 0.4f}, open ? 2.5f : 1.0f, 6.0f);
            }
            break;
        }
        case QuestKind::Herbs: {
            // Moonpetals: pale stems crowned with glowing petals that pulse softly and light the grass.
            for (const net::QuestItemState& it : snapshot_.quest_items) {
                if (it.kind != 0u) {
                    continue;
                }
                const f32 ph = static_cast<f32>(it.id) * 1.7f;
                const f32 sway = 0.08f * std::sin(elapsed_ * 1.8f + ph);
                const Vec3 base = it.position;
                const Vec3 bloom = base + Vec3{sway, 0.55f, 0.0f};
                renderer_->draw(shape_box_,
                                glm::translate(Mat4{1.0f}, (base + bloom) * 0.5f) * orient_to(bloom - base) *
                                    glm::scale(Mat4{1.0f}, Vec3{0.025f, 0.025f, glm::length(bloom - base)}),
                                Vec4{0.36f, 0.55f, 0.32f, 1.0f});
                for (int k = 0; k < 5; ++k) {
                    const f32 a = TwoPi * static_cast<f32>(k) / 5.0f + ph;
                    renderer_->draw_emissive(shape_sphere_,
                                             glm::translate(Mat4{1.0f}, bloom + Vec3{std::cos(a) * 0.09f, 0.0f, std::sin(a) * 0.09f}) *
                                                 glm::scale(Mat4{1.0f}, Vec3{0.12f, 0.04f, 0.12f}),
                                             Vec4{0.7f, 0.88f, 1.0f, 1.0f});
                }
                const f32 pulse = 0.75f + 0.25f * std::sin(elapsed_ * 2.6f + ph);
                renderer_->draw_sprite(bloom, 0.35f * pulse, Vec4{0.6f, 0.85f, 1.0f, 0.55f + 0.3f * night}, 0.3f);
                fx_light(bloom, Vec3{0.55f, 0.8f, 1.0f}, (0.8f + 1.6f * night) * pulse, 4.0f);
                if (frand() < 0.06f) {
                    emit(bloom + rand_dir() * 0.2f, Vec3{0.0f, frand(0.3f, 0.7f), 0.0f}, Vec4{0.7f, 0.9f, 1.0f, 0.8f}, 1.2f, 0.05f, 1, -0.1f);
                }
            }
            break;
        }
    }
}

} // namespace alryn::game
