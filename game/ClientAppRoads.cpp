// ClientApp - life on the roads (Game/Wayfarer.h): the wayfarers' kit (packs, staffs, a pilgrim's
// lantern, a guard's spear), the merchant caravans rolling between towns, the roadside traveller's ERRAND
// (the "!" over them, what they ask, the goat / satchels / stuck cart, the thanks + pay), and the snow that
// falls up in the snowbound high country.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"
#include "HudStyle.h"

#include <Alryn/Game/Wayfarer.h>

namespace alryn::game {

namespace {
// An errand's colour: its "!" + site marker + HUD accents.
Vec3 errand_color(ErrandKind k) {
    switch (k) {
        case ErrandKind::LostGoat: return Vec3{0.95f, 0.88f, 0.62f};
        case ErrandKind::Brigands: return Vec3{1.0f, 0.5f, 0.3f};
        case ErrandKind::StuckCart: return Vec3{0.78f, 0.62f, 0.4f};
        case ErrandKind::LostParcels: return Vec3{0.9f, 0.75f, 0.45f};
        case ErrandKind::Wolves: return Vec3{1.0f, 0.75f, 0.35f};
    }
    return Vec3{1.0f};
}
// The index of the first bone of `part`, or -1.
int bone_of(const CharacterModel& model, BonePart part) {
    const std::vector<Bone>& bones = model.bones();
    for (usize i = 0; i < bones.size(); ++i) {
        if (bones[i].part == part) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
} // namespace

// ---- The wayfarers' kit -----------------------------------------------------------------------------

void ClientApp::draw_wayfarer_kit(const net::VillagerState& vl, PlayerVisual& v, const Mat4& root, const std::vector<Quat>& pose) {
    const std::vector<Mat4> jm = v.model.joint_matrices(root, pose);
    const Vec3 fwd{std::cos(vl.yaw), 0.0f, std::sin(vl.yaw)};
    const Vec4 wood{0.38f, 0.26f, 0.15f, 1.0f};
    const Vec4 leather{0.36f, 0.24f, 0.14f, 1.0f};
    const f32 night = 1.0f - sun_intensity_;
    const auto role = static_cast<WayfarerRole>(vl.role);
    // A pack on the back: the torso's frame, pushed out behind it (the body faces local +Z).
    auto pack = [&](const Vec3& size, const Vec4& col, bool bedroll) {
        const int t = bone_of(v.model, BonePart::Torso);
        if (t < 0) {
            return;
        }
        const Bone& b = v.model.bones()[static_cast<usize>(t)];
        const Mat4 back = jm[static_cast<usize>(t)] *
                          glm::translate(Mat4{1.0f}, b.box_center + Vec3{0.0f, 0.04f, -(b.box_size.z * 0.5f + size.z * 0.5f)});
        renderer_->draw(shape_box_, back * glm::scale(Mat4{1.0f}, size), col);
        if (bedroll) {
            renderer_->draw(shape_box_,
                            back * glm::translate(Mat4{1.0f}, Vec3{0.0f, size.y * 0.5f + 0.07f, 0.0f}) *
                                glm::scale(Mat4{1.0f}, Vec3{size.x * 1.25f, 0.13f, 0.14f}),
                            Vec4{0.62f, 0.52f, 0.36f, 1.0f});
        }
    };
    // A walking staff in the right hand, its foot planted a stride ahead.
    auto staff = [&](f32 len, const Vec4& col) {
        const Mat4 hand = hand_frame(v.model, jm, BonePart::LowerArmR);
        const Vec3 grip = Vec3{hand[3]} + fwd * 0.06f;
        const Vec3 foot{grip.x + fwd.x * 0.25f, vl.position.y, grip.z + fwd.z * 0.25f};
        const Vec3 top = grip + (grip - foot) * ((len - glm::length(grip - foot)) / std::max(glm::length(grip - foot), 0.1f));
        renderer_->draw(shape_box_,
                        glm::translate(Mat4{1.0f}, (foot + top) * 0.5f) * orient_to(top - foot) *
                            glm::scale(Mat4{1.0f}, Vec3{0.05f, 0.05f, glm::length(top - foot)}),
                        col);
        return top;
    };
    switch (vl.kind == 6 ? WayfarerRole::Traveller : role) {
        case WayfarerRole::Traveller:
            if (vl.kind == 5) {
                pack(Vec3{0.34f, 0.42f, 0.2f}, leather, true);
            }
            staff(1.6f, wood);
            break;
        case WayfarerRole::Pilgrim: {
            pack(Vec3{0.26f, 0.3f, 0.14f}, Vec4{0.55f, 0.5f, 0.42f, 1.0f}, false);
            const Vec3 top = staff(1.85f, Vec4{0.46f, 0.36f, 0.24f, 1.0f});
            renderer_->draw(shape_box_, glm::translate(Mat4{1.0f}, top) * glm::scale(Mat4{1.0f}, Vec3{0.2f, 0.04f, 0.04f}),
                            Vec4{0.5f, 0.42f, 0.26f, 1.0f}); // the pilgrim's crossbar
            // By night a lantern swings from the other hand, lighting the road.
            if (night > 0.25f) {
                const Mat4 lh = hand_frame(v.model, jm, BonePart::LowerArmL);
                const Vec3 at = Vec3{lh[3]} + Vec3{0.0f, -0.22f + 0.03f * std::sin(elapsed_ * 4.0f + vl.position.x), 0.0f};
                renderer_->draw(shape_box_, glm::translate(Mat4{1.0f}, at + Vec3{0.0f, 0.15f, 0.0f}) * glm::scale(Mat4{1.0f}, Vec3{0.16f, 0.04f, 0.16f}),
                                Vec4{0.14f, 0.12f, 0.1f, 1.0f});
                const f32 g = 0.9f + 0.1f * std::sin(elapsed_ * 9.0f + vl.position.z);
                renderer_->draw_emissive(shape_box_, glm::translate(Mat4{1.0f}, at) * glm::scale(Mat4{1.0f}, Vec3{0.12f, 0.2f, 0.12f}),
                                         Vec4{1.4f * g, 1.05f * g, 0.5f * g, 1.0f});
                if (glm::length(at - local_feet()) < 45.0f) {
                    fx_light(at, Vec3{1.0f, 0.72f, 0.4f}, 1.8f * night * g, 9.0f);
                }
            }
            break;
        }
        case WayfarerRole::Merchant: {
            const int pel = bone_of(v.model, BonePart::Pelvis);
            if (pel >= 0) { // a fat purse at the hip
                const Bone& b = v.model.bones()[static_cast<usize>(pel)];
                renderer_->draw(shape_box_,
                                jm[static_cast<usize>(pel)] * glm::translate(Mat4{1.0f}, b.box_center + Vec3{b.box_size.x * 0.55f, -0.05f, 0.04f}) *
                                    glm::scale(Mat4{1.0f}, Vec3{0.12f, 0.16f, 0.12f}),
                                leather);
            }
            break;
        }
        case WayfarerRole::Guard:
            draw_held_spear(v.model, v.model.bone_matrices(root, pose));
            break;
        case WayfarerRole::Adventurer: {
            pack(Vec3{0.3f, 0.36f, 0.18f}, leather, (vl.id & 1u) != 0u);
            // A blade slung across the back, or a bow - the band's trade.
            const int t = bone_of(v.model, BonePart::Torso);
            if (t >= 0) {
                const Bone& b = v.model.bones()[static_cast<usize>(t)];
                const Mat4 back = jm[static_cast<usize>(t)] *
                                  glm::translate(Mat4{1.0f}, b.box_center + Vec3{0.0f, 0.1f, -(b.box_size.z * 0.5f + 0.24f)}) *
                                  glm::rotate(Mat4{1.0f}, (vl.id & 2u) != 0u ? 0.7f : -0.7f, Vec3{0.0f, 0.0f, 1.0f});
                renderer_->draw(shape_box_, back * glm::scale(Mat4{1.0f}, Vec3{0.06f, 1.0f, 0.03f}),
                                (vl.id & 4u) != 0u ? Vec4{0.7f, 0.72f, 0.78f, 1.0f} : wood);
            }
            break;
        }
    }
}

// ---- Merchant caravans ------------------------------------------------------------------------------

void ClientApp::update_caravans(Timestep dt) {
    if (!have_snapshot_) {
        caravan_smooth_.clear();
        return;
    }
    constexpr f32 tau = 0.08f;
    const f32 a = 1.0f - std::exp(-dt.seconds / tau);
    for (const net::CaravanState& c : snapshot_.caravans) {
        CaravanSmooth& s = caravan_smooth_[c.id];
        if (!s.init) {
            s.pos = c.position;
            s.beast = c.beast_pos;
            s.prev_beast = c.beast_pos;
            s.init = true;
            continue;
        }
        const Vec3 prev = s.pos;
        s.pos += (c.position - s.pos) * a;
        s.beast += (c.beast_pos - s.beast) * a;
        const VehicleType& vt = vehicle_type(c.type);
        s.roll -= glm::dot(Vec2{s.pos.x - prev.x, s.pos.z - prev.z}, Vec2{std::cos(c.yaw), std::sin(c.yaw)}) / vt.wheel_radius();
        s.gait += glm::length(Vec2{s.beast.x - s.prev_beast.x, s.beast.z - s.prev_beast.z}) * 2.1f;
        s.prev_beast = s.beast;
    }
    for (auto it = caravan_smooth_.begin(); it != caravan_smooth_.end();) {
        const bool live = std::any_of(snapshot_.caravans.begin(), snapshot_.caravans.end(),
                                      [&](const net::CaravanState& c) { return c.id == it->first; });
        it = live ? std::next(it) : caravan_smooth_.erase(it);
    }
}

void ClientApp::draw_caravans() {
    if (renderer_ == nullptr || !have_snapshot_) {
        return;
    }
    const f32 night = 1.0f - sun_intensity_;
    for (const net::CaravanState& c : snapshot_.caravans) {
        const auto it = caravan_smooth_.find(c.id);
        if (it == caravan_smooth_.end()) {
            continue;
        }
        const CaravanSmooth& s = it->second;
        const VehicleType& vt = vehicle_type(c.type);
        // The cart, tilted to the road under it, its wheels turning, crates + sacks heaped in the bed.
        const Vec2 fwd{std::cos(c.yaw), std::sin(c.yaw)};
        const Vec2 lat{-fwd.y, fwd.x};
        auto h = [&](const Vec2& o) {
            return std::max(worldgen::height(s.pos.x + o.x, s.pos.z + o.y, world_seed_), roads::bridge_height(s.pos.x + o.x, s.pos.z + o.y, world_seed_));
        };
        const f32 pitch = glm::clamp(std::atan((h(fwd * 0.75f) - h(fwd * -0.75f)) / 1.5f), -0.5f, 0.5f);
        const f32 roll = glm::clamp(-std::atan((h(lat * 0.75f) - h(lat * -0.75f)) / 1.5f), -0.5f, 0.5f);
        const Mat4 m = glm::translate(Mat4{1.0f}, s.pos) * glm::rotate(Mat4{1.0f}, -c.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                       glm::rotate(Mat4{1.0f}, pitch, Vec3{0.0f, 0.0f, 1.0f}) * glm::rotate(Mat4{1.0f}, roll, Vec3{1.0f, 0.0f, 0.0f});
        renderer_->draw(vehicle_meshes_[c.type % vehicle_meshes_.size()], m);
        const f32 wscale = vt.wheel_radius() / kWagonWheelRadius;
        for (const Vec3& w : vt.wheels()) {
            renderer_->draw(wagon_wheel_mesh_, m * glm::translate(Mat4{1.0f}, w) * glm::rotate(Mat4{1.0f}, s.roll, Vec3{0.0f, 0.0f, 1.0f}) *
                                                   glm::scale(Mat4{1.0f}, Vec3{wscale}));
        }
        const CargoBed bed = vt.bed();
        const Vec3 bc = (bed.lo + bed.hi) * 0.5f;
        const Vec3 be = (bed.hi - bed.lo) * 0.5f;
        for (int k = 0; k < 4; ++k) { // the load, heaped in the bed
            const f32 fx = (static_cast<f32>(k % 2) - 0.5f) * be.x * 0.9f;
            const f32 fz = (static_cast<f32>(k / 2) - 0.5f) * be.z * 0.8f;
            const Mat4 cm = m * glm::translate(Mat4{1.0f}, Vec3{bc.x + fx, bed.lo.y, bc.z + fz}) *
                            glm::rotate(Mat4{1.0f}, static_cast<f32>(c.id + k) * 0.7f, Vec3{0.0f, 1.0f, 0.0f});
            if (c.load == 1 && k % 2 == 0) {
                renderer_->draw(cargo_casks_mesh_, cm);
            } else if (c.load == 2 && k == 1) {
                renderer_->draw(cargo_weapons_mesh_, cm);
            } else {
                renderer_->draw(goods_mesh_, cm * glm::translate(Mat4{1.0f}, Vec3{0.0f, kCargoHalf, 0.0f}));
            }
        }
        // A covering tarp over the load on the bigger wagon.
        if (c.type == 1u) {
            renderer_->draw(shape_box_,
                            m * glm::translate(Mat4{1.0f}, Vec3{bc.x, bed.lo.y + 0.55f, bc.z}) *
                                glm::scale(Mat4{1.0f}, Vec3{be.x * 1.6f, 0.34f, be.z * 1.7f}),
                            Vec4{0.7f, 0.62f, 0.46f, 1.0f});
        }
        // The beast in the traces, with its own gait.
        if (c.beast == 1u) {
            const Mat4 base = glm::translate(Mat4{1.0f}, s.beast) * glm::rotate(Mat4{1.0f}, -c.beast_yaw, Vec3{0.0f, 1.0f, 0.0f});
            renderer_->draw(horse_body_mesh_, base);
            for (int k = 0; k < 4; ++k) {
                const f32 sign = (k == 0 || k == 3) ? 1.0f : -1.0f;
                renderer_->draw(horse_leg_mesh_, base * glm::translate(Mat4{1.0f}, kHorseLegs[k]) *
                                                     glm::rotate(Mat4{1.0f}, std::sin(s.gait) * 0.5f * sign, Vec3{0.0f, 0.0f, 1.0f}));
            }
        } else {
            const Mat4 base = glm::translate(Mat4{1.0f}, s.beast) * glm::rotate(Mat4{1.0f}, -c.beast_yaw, Vec3{0.0f, 1.0f, 0.0f});
            renderer_->draw(ox_body_mesh_, base);
            for (int k = 0; k < 4; ++k) {
                const f32 sign = (k == 0 || k == 3) ? 1.0f : -1.0f;
                renderer_->draw(ox_leg_mesh_, base * glm::translate(Mat4{1.0f}, kOxLegs[k]) *
                                                  glm::rotate(Mat4{1.0f}, std::sin(s.gait) * 0.42f * sign, Vec3{0.0f, 0.0f, 1.0f}));
            }
        }
        // The shafts from the cart to the beast's collar.
        for (const f32 side : {-1.0f, 1.0f}) {
            const Vec3 a = Vec3{m * Vec4{vt.bed().hi.x + 0.1f, 0.7f, side * 0.32f, 1.0f}};
            const Vec3 b = s.beast + Vec3{0.0f, 0.95f, 0.0f} + Vec3{-std::sin(c.beast_yaw), 0.0f, std::cos(c.beast_yaw)} * (side * 0.28f);
            renderer_->draw(shape_box_, glm::translate(Mat4{1.0f}, (a + b) * 0.5f) * orient_to(b - a) *
                                            glm::scale(Mat4{1.0f}, Vec3{0.05f, 0.05f, glm::length(b - a)}),
                            Vec4{0.36f, 0.25f, 0.14f, 1.0f});
        }
        // A lamp on its post by night, swinging as it rolls.
        if (night > 0.2f) {
            const Vec3 lamp = Vec3{m * Vec4{bed.hi.x, bed.hi.y + 0.9f, 0.0f, 1.0f}};
            renderer_->draw_emissive(shape_sphere_, glm::translate(Mat4{1.0f}, lamp) * glm::scale(Mat4{1.0f}, Vec3{0.15f}),
                                     Vec4{1.3f, 1.0f, 0.55f, 1.0f});
            if (glm::length(lamp - local_feet()) < 45.0f) {
                fx_light(lamp + Vec3{0.0f, 0.3f, 0.0f}, Vec3{1.0f, 0.78f, 0.45f}, 2.0f * night, 12.0f);
            }
        }
    }
}

// ---- Errands ----------------------------------------------------------------------------------------

const net::ErrandState* ClientApp::current_errand() const {
    if (!have_snapshot_ || snapshot_.errands.empty()) {
        return nullptr;
    }
    return &snapshot_.errands.front();
}

bool ClientApp::errand_talk() {
    const net::ErrandState* e = current_errand();
    if (e == nullptr || e->phase != static_cast<u8>(QuestPhase::Offered)) {
        return false;
    }
    if (glm::length(Vec2{e->giver.x - local_feet().x, e->giver.z - local_feet().z}) > kErrandTalkRange) {
        return false;
    }
    pending_quest_pick_ = e->id;
    quest_pick_hold_ = 8;
    if (Audio* a = audio()) {
        a->play(SfxId::UiClick);
    }
    return true;
}

void ClientApp::update_errand_fx(Timestep dt) {
    errand_banner_ = std::max(0.0f, errand_banner_ - dt.seconds);
    errand_say_t_ = std::max(0.0f, errand_say_t_ - dt.seconds);
    const net::ErrandState* e = current_errand();
    if (e == nullptr) {
        last_errand_id_ = 0;
        return;
    }
    Audio* a = audio();
    const auto kind = static_cast<ErrandKind>(e->kind);
    if (e->id != last_errand_id_) {
        last_errand_id_ = e->id;
        last_errand_phase_ = e->phase;
        last_errand_progress_ = e->progress;
        return;
    }
    if (e->phase != last_errand_phase_) {
        if (e->phase == static_cast<u8>(QuestPhase::Active)) {
            combat_text(e->giver + Vec3{0.0f, 2.2f, 0.0f}, "ERRAND TAKEN", Vec4{0.95f, 0.88f, 0.62f, 1.0f}, 22.0f);
            errand_say_ = "Thank you! Hurry!";
            errand_say_t_ = 3.0f;
            if (a != nullptr) {
                a->play(SfxId::Horn, 0.5f, 1.2f);
            }
        } else if (e->phase == static_cast<u8>(QuestPhase::Complete)) {
            errand_banner_ = 5.0f;
            errand_banner_text_ = std::format("{}  -  +{} GOLD", errand_title(kind), e->reward);
            errand_say_ = errand_thanks(kind);
            errand_say_t_ = 5.0f;
            if (a != nullptr) {
                a->play(SfxId::Fanfare, 0.8f);
                a->play(SfxId::Coin, 0.7f);
            }
            emit_burst(e->giver + Vec3{0.0f, 1.4f, 0.0f}, Vec4{1.0f, 0.86f, 0.4f, 1.0f}, 26, 3.5f, 1.0f, 0.1f, 1, 4.0f, 4.0f);
        }
        last_errand_phase_ = e->phase;
    }
    if (e->progress > last_errand_progress_ && e->phase == static_cast<u8>(QuestPhase::Active)) {
        combat_text(local_feet(), std::format("{} / {}", e->progress, e->goal), Vec4{0.95f, 0.88f, 0.62f, 1.0f}, 20.0f);
        if (a != nullptr && kind != ErrandKind::Brigands && kind != ErrandKind::Wolves) {
            a->play(SfxId::Coin, 0.5f, 1.3f);
        }
    }
    last_errand_progress_ = e->progress;
}

void ClientApp::draw_errand_world() {
    const net::ErrandState* e = current_errand();
    if (e == nullptr || renderer_ == nullptr || terrain_ == nullptr) {
        return;
    }
    const Vec3 feet = local_feet();
    if (glm::length(Vec2{e->giver.x - feet.x, e->giver.z - feet.z}) > 140.0f &&
        glm::length(Vec2{e->site.x - feet.x, e->site.z - feet.z}) > 140.0f) {
        return;
    }
    const f32 night = 1.0f - sun_intensity_;
    // The traveller's little roadside stop: a bundle at their feet and, by night, a small fire.
    if (static_cast<ErrandKind>(e->kind) != ErrandKind::StuckCart) {
        const Vec3 at = e->giver + Vec3{0.7f, 0.0f, -0.5f};
        renderer_->draw(shape_sphere_, glm::translate(Mat4{1.0f}, at + Vec3{0.0f, 0.22f, 0.0f}) * glm::scale(Mat4{1.0f}, Vec3{0.5f, 0.45f, 0.42f}),
                        Vec4{0.62f, 0.54f, 0.38f, 1.0f});
        if (night > 0.3f) {
            const Vec3 fire = e->giver + Vec3{-1.2f, 0.0f, 0.9f};
            const f32 flick = 0.85f + 0.15f * std::sin(elapsed_ * 11.0f + e->giver.x);
            renderer_->draw_sprite(fire + Vec3{0.0f, 0.35f, 0.0f}, 0.42f * flick, Vec4{1.0f, 0.55f, 0.2f, 0.75f}, 0.3f);
            renderer_->draw_sprite(fire + Vec3{0.0f, 0.25f, 0.0f}, 0.2f, Vec4{1.0f, 0.88f, 0.55f, 1.0f}, 0.8f);
            fx_light(fire + Vec3{0.0f, 0.8f, 0.0f}, Vec3{1.0f, 0.58f, 0.25f}, 1.8f * night * flick, 9.0f);
        }
    }
    for (const net::QuestItemState& it : snapshot_.quest_items) {
        switch (it.kind) {
            case 2: { // the goat, trotting at her new friend's heels (or cropping the grass where she ran)
                GoatGait& g = goat_gait_[it.id];
                const f32 moved = g.init ? glm::length(Vec2{it.position.x - g.prev.x, it.position.z - g.prev.z}) : 0.0f;
                g.phase += moved * 6.0f;
                g.prev = it.position;
                g.init = true;
                const f32 bob = std::abs(std::sin(g.phase)) * 0.04f * glm::clamp(moved * 30.0f, 0.0f, 1.0f);
                const Mat4 base = glm::translate(Mat4{1.0f}, it.position + Vec3{0.0f, bob, 0.0f}) *
                                  glm::rotate(Mat4{1.0f}, -it.yaw, Vec3{0.0f, 1.0f, 0.0f});
                renderer_->draw(goat_body_mesh_, base);
                for (int k = 0; k < 4; ++k) {
                    const f32 sign = (k == 0 || k == 3) ? 1.0f : -1.0f;
                    renderer_->draw(goat_leg_mesh_, base * glm::translate(Mat4{1.0f}, kGoatLegs[k]) *
                                                        glm::rotate(Mat4{1.0f}, std::sin(g.phase) * 0.55f * sign, Vec3{0.0f, 0.0f, 1.0f}));
                }
                if (it.state == 0u) { // a little halo so she can be found in the long grass
                    const f32 pulse = 0.5f + 0.5f * std::sin(elapsed_ * 3.0f);
                    sprite_circle(it.position + Vec3{0.0f, 0.05f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, 1.1f, 24, 0.04f,
                                  Vec4{0.95f, 0.88f, 0.62f, 0.3f + 0.3f * pulse}, 0.5f);
                }
                break;
            }
            case 3: { // a spilled satchel: a strapped leather bag lying in the grass, glinting
                const Mat4 base = glm::translate(Mat4{1.0f}, it.position) * glm::rotate(Mat4{1.0f}, -it.yaw, Vec3{0.0f, 1.0f, 0.0f});
                renderer_->draw(shape_box_, base * glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.16f, 0.0f}) * glm::scale(Mat4{1.0f}, Vec3{0.46f, 0.32f, 0.2f}),
                                Vec4{0.46f, 0.3f, 0.16f, 1.0f});
                renderer_->draw(shape_box_, base * glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.3f, 0.06f}) * glm::scale(Mat4{1.0f}, Vec3{0.48f, 0.08f, 0.14f}),
                                Vec4{0.36f, 0.22f, 0.12f, 1.0f}); // the flap
                renderer_->draw(shape_box_, base * glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.3f, 0.0f}) * glm::scale(Mat4{1.0f}, Vec3{0.06f, 0.36f, 0.24f}),
                                Vec4{0.26f, 0.16f, 0.09f, 1.0f}); // the strap
                const f32 tw = 0.5f + 0.5f * std::sin(elapsed_ * 4.0f + static_cast<f32>(it.id));
                renderer_->draw_sprite(it.position + Vec3{0.0f, 0.5f, 0.0f}, 0.16f + 0.08f * tw, Vec4{1.0f, 0.9f, 0.6f, 0.6f * tw}, 0.6f);
                break;
            }
            case 4: { // the stuck cart, sunk to the axle at one corner in a puddle of mud (until it's dug out)
                const VehicleType& vt = vehicle_type(0);
                const bool stuck = it.state == 0u;
                Mat4 m = glm::translate(Mat4{1.0f}, it.position) * glm::rotate(Mat4{1.0f}, -it.yaw, Vec3{0.0f, 1.0f, 0.0f});
                if (stuck) {
                    m = m * glm::translate(Mat4{1.0f}, Vec3{0.0f, -0.18f, 0.0f}) * glm::rotate(Mat4{1.0f}, 0.2f, Vec3{1.0f, 0.0f, 0.0f}) *
                        glm::rotate(Mat4{1.0f}, -0.08f, Vec3{0.0f, 0.0f, 1.0f});
                }
                renderer_->draw(vehicle_meshes_[0], m);
                const f32 wscale = vt.wheel_radius() / kWagonWheelRadius;
                for (const Vec3& w : vt.wheels()) {
                    renderer_->draw(wagon_wheel_mesh_, m * glm::translate(Mat4{1.0f}, w) * glm::scale(Mat4{1.0f}, Vec3{wscale}));
                }
                for (int k = 0; k < 3; ++k) { // sacks still in the bed
                    renderer_->draw(goods_mesh_, m * glm::translate(Mat4{1.0f}, Vec3{-0.4f + 0.4f * static_cast<f32>(k), vt.bed().lo.y + kCargoHalf, 0.0f}));
                }
                if (stuck) {
                    const Vec3 wheel = Vec3{m * Vec4{vt.wheels().empty() ? Vec3{0.0f} : vt.wheels().front(), 1.0f}};
                    renderer_->draw_transparent(shape_sphere_,
                                                glm::translate(Mat4{1.0f}, Vec3{wheel.x, it.position.y - 0.02f, wheel.z}) *
                                                    glm::scale(Mat4{1.0f}, Vec3{2.2f, 0.12f, 1.8f}),
                                                Vec4{0.22f, 0.17f, 0.11f, 0.85f}); // the mud
                }
                break;
            }
            default:
                break;
        }
    }
    // Forget the gaits of goats no longer in the snapshot.
    for (auto it = goat_gait_.begin(); it != goat_gait_.end();) {
        const bool live = std::any_of(snapshot_.quest_items.begin(), snapshot_.quest_items.end(),
                                      [&](const net::QuestItemState& q) { return q.id == it->first; });
        it = live ? std::next(it) : goat_gait_.erase(it);
    }
}

void ClientApp::draw_waypoint(ui::DrawList& draw, f32 W, f32 H, f32 ts, const Vec3& at, const Vec4& col, f32 hide_within) {
    const Vec3 feet = local_feet();
    const f32 d = glm::length(Vec2{at.x - feet.x, at.z - feet.z});
    Vec2 sp;
    const bool on = world_to_screen(at + Vec3{0.0f, 4.0f + 0.25f * std::sin(elapsed_ * 2.4f), 0.0f}, W, H, sp) && sp.x > 30.0f &&
                    sp.x < W - 30.0f && sp.y > 30.0f && sp.y < H - 30.0f;
    if (on && d > hide_within) {
        const f32 r = ts * 0.5f;
        draw.glow(Vec4{sp.x - r * 2.2f, sp.y - r * 2.2f, r * 4.4f, r * 4.4f}, hud::alpha(col, 0.35f), Vec4{Vec3{col}, 0.0f});
        draw.line(Vec2{sp.x, sp.y - r}, Vec2{sp.x + r, sp.y}, 3.0f, col);
        draw.line(Vec2{sp.x + r, sp.y}, Vec2{sp.x, sp.y + r}, 3.0f, col);
        draw.line(Vec2{sp.x, sp.y + r}, Vec2{sp.x - r, sp.y}, 3.0f, col);
        draw.line(Vec2{sp.x - r, sp.y}, Vec2{sp.x, sp.y - r}, 3.0f, col);
        hud::text(draw, Vec2{sp.x, sp.y + r + 5.0f}, std::format("{} M", static_cast<int>(d)), ts * 0.5f, col, ui::TextAlign::Center);
    } else if (!on) {
        // Off-screen: a pointer pinned to the screen edge in its direction.
        Vec2 dir{0.0f, -1.0f};
        const Vec4 clip = camera_.view_projection() * Vec4{at, 1.0f};
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
        const f32 k = std::min((W * 0.5f - 46.0f) / std::max(std::abs(dir.x), 1e-3f), (H * 0.5f - 46.0f) / std::max(std::abs(dir.y), 1e-3f));
        const Vec2 p = c + dir * k;
        const Vec2 side{-dir.y, dir.x};
        const f32 s = ts * 0.55f;
        draw.line(p + dir * s, p - dir * s * 0.6f + side * s * 0.8f, 3.5f, col);
        draw.line(p + dir * s, p - dir * s * 0.6f - side * s * 0.8f, 3.5f, col);
        hud::text(draw, p - dir * s * 1.8f - Vec2{0.0f, s * 0.5f}, std::format("{} M", static_cast<int>(d)), ts * 0.48f, col, ui::TextAlign::Center);
    }
}

void ClientApp::draw_errand_hud(ui::DrawList& draw, f32 W, f32 H, f32 ts) {
    const net::ErrandState* e = current_errand();
    const Vec3 feet = local_feet();
    if (e != nullptr) {
        const auto kind = static_cast<ErrandKind>(e->kind);
        const Vec4 col{errand_color(kind), 1.0f};
        const f32 dg = glm::length(Vec2{e->giver.x - feet.x, e->giver.z - feet.z});
        // A "!" over a traveller who needs help (a "?" while the help's under way) - seen from down the road.
        if (e->phase != static_cast<u8>(QuestPhase::Complete) && dg < 75.0f && dg > 2.0f) {
            Vec2 sp;
            const f32 bob = 0.1f * std::sin(elapsed_ * 3.2f);
            if (world_to_screen(e->giver + Vec3{0.0f, 2.45f + bob, 0.0f}, W, H, sp)) {
                const f32 r = ts * 0.55f;
                draw.glow(Vec4{sp.x - r * 2.0f, sp.y - r * 2.0f, r * 4.0f, r * 4.0f}, hud::alpha(col, 0.35f), Vec4{Vec3{col}, 0.0f});
                hud::medallion(draw, sp, r, Vec4{0.08f, 0.05f, 0.03f, 1.0f});
                hud::text(draw, Vec2{sp.x, sp.y - ts * 0.38f}, e->phase == static_cast<u8>(QuestPhase::Offered) ? "!" : "?", ts * 0.78f,
                          hud::kGold, ui::TextAlign::Center, ui::FontFace::Display);
            }
        }
        // Standing by them while they plead: what they ask, what they'll pay, and how to say yes.
        if (e->phase == static_cast<u8>(QuestPhase::Offered) && dg < kErrandTalkRange + 2.5f) {
            const ui::Theme& th = ui::theme();
            const f32 pw = glm::clamp(W * 0.34f, 360.0f, 540.0f);
            const std::vector<std::string> lines = hud::wrap(std::format("\"{}\"", errand_plea(kind)), ts * 0.56f, pw - 44.0f);
            const f32 ph = ts * 2.2f + static_cast<f32>(lines.size()) * ts * 0.78f + ts * 1.9f;
            const Vec4 card{(W - pw) * 0.5f, H * 0.62f - ph, pw, ph};
            draw.shadow(card, 12.0f, 18.0f, Vec4{0.0f, 0.0f, 0.0f, 0.6f}, Vec2{0.0f, 6.0f});
            draw.gradient(card, th.panel, th.panel_bottom, 12.0f, th.panel_border, 1.75f);
            f32 iy = card.y + 14.0f;
            const f32 ix = card.x + 22.0f;
            hud::text(draw, Vec2{ix, iy}, "A TRAVELLER BY THE ROAD", ts * 0.5f, th.text_muted);
            iy += ts * 0.85f;
            hud::text(draw, Vec2{ix, iy}, errand_title(kind), ts * 0.8f, hud::kGold, ui::TextAlign::Left, ui::FontFace::Display);
            iy += ts * 1.25f;
            for (const std::string& l : lines) {
                hud::text(draw, Vec2{ix, iy}, l, ts * 0.56f, th.text);
                iy += ts * 0.78f;
            }
            iy += ts * 0.25f;
            hud::coin(draw, Vec2{ix + ts * 0.3f, iy + ts * 0.42f}, ts * 0.3f);
            hud::text(draw, Vec2{ix + ts * 0.75f, iy + ts * 0.12f}, std::format("{}", e->reward), ts * 0.6f, hud::kGold);
            const char* help = dg < kErrandTalkRange ? "[E] I'LL HELP" : "COME CLOSER TO TALK";
            hud::rich(draw, Vec2{card.x + pw - 22.0f - hud::rich_width(help, ts * 0.62f), iy + ts * 0.08f}, help, ts * 0.62f,
                      dg < kErrandTalkRange ? hud::kGood : th.text_muted);
        }
        // Under way: the waypoint to where the help is needed (back to the owner once the goat's found).
        if (e->phase == static_cast<u8>(QuestPhase::Active)) {
            Vec3 goal = e->site;
            bool goat_led = false;
            for (const net::QuestItemState& it : snapshot_.quest_items) {
                if (it.kind == 2u && it.state == 1u) {
                    goat_led = true;
                }
            }
            if (goat_led) {
                goal = e->giver;
            }
            draw_waypoint(draw, W, H, ts, goal, col, kind == ErrandKind::LostParcels ? 4.0f : 7.0f);
            // E prompts beside the goat / a satchel in reach; Q by the stuck wheel.
            for (const net::QuestItemState& it : snapshot_.quest_items) {
                const f32 d = glm::length(Vec2{it.position.x - feet.x, it.position.z - feet.z});
                const char* hint = nullptr;
                if (it.kind == 2u && it.state == 0u && d < kGoatPickRange + 0.4f) {
                    hint = "[E] TAKE HER BY THE ROPE";
                } else if (it.kind == 3u && it.state == 0u && d < kParcelPickRange + 0.4f) {
                    hint = "[E] PICK UP THE SATCHEL";
                } else if (it.kind == 4u && it.state == 0u && d < kStuckDigReach + 2.0f) {
                    hint = "AIM AT THE WHEEL  -  [Q] DIG";
                }
                Vec2 sp;
                if (hint != nullptr && world_to_screen(it.position + Vec3{0.0f, 1.3f, 0.0f}, W, H, sp)) {
                    hud::rich(draw, Vec2{sp.x - hud::rich_width(hint, ts * 0.6f) * 0.5f, sp.y}, hint, ts * 0.6f, hud::kGold);
                    break;
                }
            }
        }
        // What the traveller says (taking it on, the thanks), in a little bubble over their head.
        if (errand_say_t_ > 0.0f && dg < 40.0f) {
            Vec2 sp;
            if (world_to_screen(e->giver + Vec3{0.0f, 2.35f, 0.0f}, W, H, sp)) {
                const f32 fs = ts * 0.56f;
                const f32 tw = hud::width(errand_say_, fs) + 26.0f;
                const f32 a = glm::clamp(errand_say_t_ / 0.5f, 0.0f, 1.0f);
                const Vec4 bub{sp.x - tw * 0.5f, sp.y - fs * 2.4f, tw, fs * 1.8f};
                draw.rect(bub, Vec4{0.97f, 0.94f, 0.86f, 0.95f * a}, Vec4{0.2f, 0.14f, 0.08f, a}, 1.5f, 8.0f);
                hud::text(draw, Vec2{sp.x, bub.y + fs * 0.4f}, errand_say_, fs, Vec4{0.18f, 0.12f, 0.07f, a}, ui::TextAlign::Center);
            }
        }
    }
    if (errand_banner_ > 0.0f) {
        const f32 bs = glm::clamp(H * 0.042f, 22.0f, 48.0f);
        hud::banner(draw, W * 0.5f, H * 0.3f, errand_banner_text_, bs, hud::kGood);
    }
}

// ---- Snow in the high country -----------------------------------------------------------------------

void ClientApp::draw_snowfall() {
    if (renderer_ == nullptr || world_seed_ == 0) {
        return;
    }
    const Vec3 feet = local_feet();
    // Snow falls where the ground lies under it (the peaks + plateaus), thickest in a storm; eased so it
    // thickens + thins as you climb in and out of the snowline.
    const f32 want = worldgen::snow_cover(worldgen::base_height(feet.x, feet.z, world_seed_)) * (0.45f + 0.55f * weather_amt_);
    snow_amt_ += (want - snow_amt_) * std::min(1.0f, frame_dt_ * 0.8f);
    if (snow_amt_ < 0.02f) {
        return;
    }
    auto hcell = [](int x, int z, int s) {
        u32 v = static_cast<u32>(x * 73856093) ^ static_cast<u32>(z * 19349663) ^ static_cast<u32>(s * 83492791);
        v ^= v >> 13;
        v *= 0x2545F491u;
        v ^= v >> 16;
        return static_cast<f32>((v >> 8) & 0xFFFFu) / 65535.0f;
    };
    const f32 t = elapsed_;
    constexpr f32 cs = 1.1f;
    constexpr int cr = 11;
    constexpr f32 ceil_h = 12.0f, fall_h = 15.0f;
    const f32 wdir = t * 0.04f;
    const Vec2 wind = Vec2{std::cos(wdir), std::sin(wdir)} * (0.4f + 2.2f * weather_amt_);
    const Vec4 col{0.92f, 0.95f, 1.0f, 0.55f + 0.35f * snow_amt_};
    const int pcx = static_cast<int>(std::floor(feet.x / cs));
    const int pcz = static_cast<int>(std::floor(feet.z / cs));
    for (int dz = -cr; dz <= cr; ++dz) {
        for (int dx = -cr; dx <= cr; ++dx) {
            const int cx = pcx + dx, cz = pcz + dz;
            if (hcell(cx, cz, 5) > 0.15f + 0.85f * snow_amt_) {
                continue;
            }
            const f32 speed = 1.2f + 0.8f * hcell(cx, cz, 9); // flakes drift down slowly
            const f32 fallen = std::fmod(t * speed + hcell(cx, cz, 17) * fall_h, fall_h);
            const f32 wy = feet.y + ceil_h - fallen;
            if (wy < feet.y - 2.5f) {
                continue;
            }
            const f32 age = fallen / speed;
            const f32 ph = hcell(cx, cz, 23) * TwoPi;
            const Vec3 pos{(static_cast<f32>(cx) + hcell(cx, cz, 11)) * cs + wind.x * age * 0.3f + 0.35f * std::sin(t * 1.3f + ph),
                           wy,
                           (static_cast<f32>(cz) + hcell(cx, cz, 13)) * cs + wind.y * age * 0.3f + 0.35f * std::cos(t * 1.1f + ph)};
            renderer_->draw_sprite(pos, 0.045f + 0.03f * hcell(cx, cz, 29), col, 0.8f);
        }
    }
}

// ---- Scripted screenshots ---------------------------------------------------------------------------

void ClientApp::dev_road_setup() {
    const std::string_view scr = dev_screen();
    if (dev_road_done_ || !have_snapshot_ || dev_setup_wait_ < 1.0f) {
        return;
    }
    if (scr != "city" && scr != "snowtown" && scr != "hamlet" && scr != "village" && scr != "road" && scr != "errand" &&
        scr != "caravan") {
        dev_road_done_ = true;
        return;
    }
    if (!host_local_ || !local_server_.running()) {
        dev_road_done_ = true;
        return;
    }
    std::lock_guard<std::mutex> lock(server_mutex_);
    const Vec3 feet = local_feet();
    // ALRYN_SCREEN=city / snowtown / hamlet / village: stand in the nearest such settlement (searched
    // outward over the grid; ALRYN_SEED picks the world), on a street a little off its centre.
    if (scr == "city" || scr == "snowtown" || scr == "hamlet" || scr == "village") {
        const int fcx = static_cast<int>(std::floor(feet.x / worldgen::village_cell));
        const int fcz = static_cast<int>(std::floor(feet.z / worldgen::village_cell));
        std::optional<worldgen::Village> pick;
        for (int r = 0; r <= 24 && !pick; ++r) {
            for (int dz = -r; dz <= r && !pick; ++dz) {
                for (int dx = -r; dx <= r && !pick; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dz)) != r) {
                        continue;
                    }
                    const auto v = worldgen::village_at(fcx + dx, fcz + dz, world_seed_);
                    if (!v) {
                        continue;
                    }
                    const bool ok = scr == "city"       ? v->tier == worldgen::TownTier::City
                                    : scr == "snowtown" ? v->snowy && v->tier != worldgen::TownTier::Hamlet
                                    : scr == "hamlet"   ? v->tier == worldgen::TownTier::Hamlet
                                                        : v->tier == worldgen::TownTier::Village;
                    if (ok) {
                        pick = v;
                    }
                }
            }
        }
        if (pick) {
            // On one of its streets a little out from the centre (streets keep clear of every building).
            Vec2 p = pick->center;
            for (const detail::Street& st : detail::village_streets(*pick, world_seed_)) {
                const Vec2 mid = (st.a + st.b) * 0.5f;
                if (glm::length(mid - pick->center) > detail::plaza_half(*pick) + 4.0f) {
                    p = mid;
                    break;
                }
            }
            local_server_.debug_place_player(my_id_, Vec3{p.x, worldgen::height(p.x, p.y, world_seed_) + 0.6f, p.y});
            ALRYN_INFO("Dev: {} {} at {:.0f},{:.0f} (half {:.0f}, {} homes)", worldgen::town_tier_name(pick->tier),
                       pick->snowy ? "(snowbound)" : "", pick->center.x, pick->center.y, pick->half,
                       detail::cached_town_plan(*pick, world_seed_).houses.size());
        }
        dev_road_done_ = true;
        return;
    }
    // ALRYN_SCREEN=road / errand / caravan: out on the road from the start town (a little past its gate),
    // with a caravan + a band set out toward us (road), a traveller waiting just ahead (errand, kind by
    // ALRYN_ERRAND_KIND 0..4, taken at once with ALRYN_ERRAND_TAKE=1).
    if (!dev_road_placed_) {
        const auto town = worldgen::village_containing(feet.x, feet.z, world_seed_, 20.0f);
        const auto dests = town ? roads::reachable_towns(town->center, world_seed_, 1) : std::vector<worldgen::Village>{};
        if (town && !dests.empty()) {
            const std::vector<Vec2> route = roads::route_polyline(town->center, dests.front().center, world_seed_);
            f32 walked = 0.0f;
            const f32 want = town->half * 1.35f + 40.0f;
            for (usize i = 1; i < route.size(); ++i) {
                const f32 seg = glm::length(route[i] - route[i - 1]);
                if (walked + seg >= want) {
                    const Vec2 p = glm::mix(route[i - 1], route[i], (want - walked) / seg);
                    dev_road_dir_ = glm::normalize(route[i] - route[i - 1]);
                    local_server_.debug_place_player(my_id_, Vec3{p.x, worldgen::height(p.x, p.y, world_seed_) + 0.6f, p.y});
                    face_yaw_ = std::atan2(dev_road_dir_.y, dev_road_dir_.x);
                    break;
                }
                walked += seg;
            }
        }
        dev_road_placed_ = true;
        dev_setup_wait_ = 0.5f; // let the move land before setting the scene
        return;
    }
    if (scr == "errand") {
        int kind = 0;
        if (const char* k = std::getenv("ALRYN_ERRAND_KIND"); k != nullptr) {
            kind = std::atoi(k) % kErrandKinds;
        }
        local_server_.set_road_life(false);
        if (local_server_.debug_spawn_errand(static_cast<ErrandKind>(kind), feet, dev_road_dir_)) {
            // Walk right up to the traveller (or take it on at once, for the under-way shots).
            const GameServer::Errand& e = local_server_.errands().front();
            const Vec3 g = e.giver.position;
            const Vec2 back = glm::normalize(Vec2{feet.x - g.x, feet.z - g.z});
            const Vec2 p = Vec2{g.x, g.z} + back * 2.4f;
            local_server_.debug_place_player(my_id_, Vec3{p.x, worldgen::height(p.x, p.y, world_seed_) + 0.6f, p.y});
            if (const char* t = std::getenv("ALRYN_ERRAND_TAKE"); t != nullptr && t[0] == '1') {
                pending_quest_pick_ = e.id;
                quest_pick_hold_ = 8;
            }
        }
    } else {
        const bool c = local_server_.debug_spawn_wayfarers(feet, true, 16.0f);
        const bool b = local_server_.debug_spawn_wayfarers(feet, false, 9.0f);
        ALRYN_INFO("Dev: road traffic - caravan {}, band {}", c ? "out" : "none", b ? "out" : "none");
    }
    dev_road_done_ = true;
}

} // namespace alryn::game
