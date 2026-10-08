// ClientApp - enemies, villagers, props and fire rendering + their visuals.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

namespace alryn::game {

CharacterAppearance ClientApp::enemy_look() {
    CharacterAppearance a;
    a.skin = 3;                  // a weathered, grubby complexion (mostly hidden under the hood + mask)
    a.hair_color = 1;            // brown
    a.eyes = EyeStyle::Sharp;
    a.ears = EarStyle::Round;    // human bandits, not goblins
    a.hair = HairStyle::Short;
    return a;
}

namespace {
// Melee raiders wear the Brigand kit (hood + face-mask, ragged jerkin); archers wear the Outlaw kit
// (deep hood + scarf + back quiver), so ranged bandits read distinctly from the melee cutthroats.
OutfitKind bandit_outfit_for(u8 kind) {
    return kind == 3u ? OutfitKind::Outlaw : OutfitKind::Brigand;
}
// A crude scavenged weapon gripped in a bandit's hand, chosen by kind (None = hands are full with a
// torch / satchel / mending orb, drawn separately below).
WeaponType bandit_weapon(u8 kind) {
    switch (kind) {
        case 2u: return WeaponType::Mace;             // brute - a heavy maul
        case 3u: return WeaponType::Bow;              // archer
        case kEnemyShield: return WeaponType::Sword;  // shield-bearer (+ the big front shield below)
        case kEnemyWarlord: return WeaponType::Sword; // champion - a notched blade
        case 1u:                                      // torch-bearer holds the torch
        case kEnemySapper:                            // sapper's hands are on the satchel charge
        case kEnemyHealer: return WeaponType::None;   // healer floats the mending orb
        default: return WeaponType::Dagger;           // grunt - a crude shiv/cleaver
    }
}
// A subtle grimy tint per kind so bandits still read as hostile (the outfit carries the identity now,
// so this is a gentle wash, not the old tomato-red player recolour). Health bars mark them too.
Vec3 bandit_tint(u8 kind) {
    switch (kind) {
        case 1u: return Vec3{1.15f, 0.92f, 0.72f};        // torch-bearer - warmed by the flame
        case 2u: return Vec3{1.0f, 0.74f, 0.72f};         // brute - a ruddy, heavy bruiser
        case 3u: return Vec3{0.92f, 1.0f, 0.9f};          // archer/outlaw - a cold woodland cast
        case kEnemyHealer: return Vec3{0.9f, 1.0f, 0.94f}; // healer - a sickly pallor
        case kEnemyWarlord: return Vec3{1.08f, 0.72f, 0.72f}; // warlord - a deep crimson menace
        case kEnemyWolf:
        case kEnemyAlpha: return Vec3{0.86f, 0.9f, 1.0f};   // wolves - a cold grey coat (not the warm grade's tan)
        default: return Vec3{1.0f, 0.9f, 0.88f};          // grunts - a grimy warm neutral
    }
}
// The shortest rotation taking direction `a` onto `b` (both unit length).
Quat from_to(const Vec3& a, const Vec3& b) {
    const f32 c = glm::dot(a, b);
    if (c < -0.9999f) {
        return glm::angleAxis(Pi, Vec3{1.0f, 0.0f, 0.0f});
    }
    const Vec3 ax = glm::cross(a, b);
    return glm::normalize(Quat{1.0f + c, ax.x, ax.y, ax.z});
}
// A felled foe's size (matching the server-side bulk): brutes + warlords tower, an alpha wolf looms.
f32 enemy_scale(u8 kind) {
    return kind == 2 ? 1.5f : kind == kEnemyWarlord ? 1.28f : kind == kEnemyAlpha ? 1.35f : 1.0f;
}
// Where a character's eyes sit in the world (matching CharacterModel::add_features), so a raider's
// ember-lit eyes can glint out from under its hood.
std::array<Vec3, 2> eye_points(const CharacterModel& m, const std::vector<Mat4>& jmats) {
    const int hi = m.bone_index(BonePart::Head);
    if (hi < 0 || static_cast<usize>(hi) >= jmats.size()) {
        return {};
    }
    const Bone& head = m.bones()[static_cast<usize>(hi)];
    const f32 r = head.box_size.x * 0.5f;
    const Vec3 c = head.box_center;
    const Mat4& J = jmats[static_cast<usize>(hi)];
    return {Vec3{J * Vec4{c.x - r * 0.38f, c.y + r * 0.1f, c.z + r * 1.02f, 1.0f}},
            Vec3{J * Vec4{c.x + r * 0.38f, c.y + r * 0.1f, c.z + r * 1.02f, 1.0f}}};
}
} // namespace

void ClientApp::update_enemy_visuals(Timestep dt) {
    if (!have_snapshot_) {
        return;
    }
    Audio* snd = audio();
    for (const net::EnemyState& en : snapshot_.enemies) {
        const auto [it, created] = enemy_visuals_.try_emplace(en.id);
        EnemyVisual& v = it->second;
        const bool beast = is_beast(en.kind);
        if (created) {
            v.last_pos = en.position;
            v.kind = en.kind;
            v.last_health = en.health;
            v.gait = static_cast<f32>(en.id % 7u);
            if (!beast) {
                const OutfitKind kind = bandit_outfit_for(en.kind);
                CharacterAppearance look = enemy_look();
                look.skin = static_cast<u8>((en.id * 7u + 2u) % 6u); // vary complexion per bandit
                v.model = CharacterModel::create(en.id ^ 0xE0E0u, look);
                Equipment eq;
                eq.outfit_tint = static_cast<u8>(en.id % 4u); // vary the cloth colour per bandit
                apply_outfit(v.model, kind, eq);              // dress them (adds decorative attachment bones)
                // Ember-lit eyes glaring out from under the hood - the whites sunk in its shadow.
                v.model.palette().glow = Vec3{1.0f, 0.42f, 0.12f};
                v.model.recolor_attachments([](const Bone& b) { return b.color == BoneColor::Eye; }, BoneColor::Glow);
                v.model.recolor_attachments([](const Bone& b) { return b.color == BoneColor::Linen; }, BoneColor::Dark);
                v.body_skin = build_body_mesh(v.model);
                v.outfit_skin = build_outfit_mesh(v.model, kind, eq);
            }
            // A raider charging into view bellows a war-cry, a wolf howls - one in a few, so a whole pack
            // or warband doesn't drown itself out.
            if (snd != nullptr && (en.id % 3u == 0u || en.kind == kEnemyWarlord || en.kind == kEnemyAlpha) &&
                glm::length(en.position - local_feet()) < 60.0f) {
                snd->play_at(beast ? SfxId::Howl : SfxId::Roar, en.position, beast ? 0.75f : 0.65f,
                             en.kind == 2 || en.kind == kEnemyAlpha ? 0.8f : frand(0.92f, 1.12f));
            }
        }
        // A fresh wound: the body blanches + recoils for a moment.
        if (en.health + 2u < v.last_health) {
            v.hurt = 1.0f;
        }
        v.hurt = std::max(0.0f, v.hurt - dt.seconds * 5.0f);
        v.last_health = en.health;
        f32 measured = 0.0f;
        if (dt.seconds > 0.0001f) {
            Vec3 d = en.position - v.last_pos;
            d.y = 0.0f;
            measured = glm::length(d) / dt.seconds;
        }
        v.speed = glm::mix(v.speed, measured, 0.3f);
        v.last_pos = en.position;
        v.last_yaw_dir = Vec3{std::cos(en.yaw), 0.0f, std::sin(en.yaw)};
        if (beast) {
            // The run cycle follows the ground covered; the jaw snaps open on a bite / a pounce and
            // curls into a snarl while crouched to spring (with a growl).
            v.gait += std::min(measured, 12.0f) * dt.seconds * 2.4f;
            const f32 jaw_goal = (en.action == 1 || en.action == 4)  ? 1.0f
                                 : en.action == 2                     ? 0.55f
                                                                      : 0.1f + 0.08f * std::sin(elapsed_ * 3.0f + static_cast<f32>(en.id));
            v.jaw += (jaw_goal - v.jaw) * std::min(1.0f, dt.seconds * 16.0f);
            if (snd != nullptr && en.action == 2 && v.last_action != 2) {
                snd->play_at(SfxId::Roar, en.position, 0.45f, en.kind == kEnemyAlpha ? 1.25f : 1.55f); // a snarl
            }
        } else {
            if (en.action == 1 && v.last_action != 1) {
                v.animator.play_swing(0.1f); // the enemy just struck - swing (from the top of the wind-up)
            }
            // The archer DRAWS its bow while it aims; the brute heaves its maul up overhead as the slam
            // winds up, and brings it crashing down on the strike.
            if (en.kind == 3u) {
                v.animator.set_charge(en.action == 2 ? 1.0f : 0.0f, 1);
            } else if (en.kind == 2u) {
                v.animator.set_charge(en.action == 2 ? 1.0f : 0.0f, 0);
                if (en.action == 3 && v.last_action != 3) {
                    v.animator.play_heavy(0);
                    const Vec3 at = en.position + v.last_yaw_dir * 1.3f;
                    emit_ring(at, Vec4{0.6f, 0.52f, 0.42f, 0.55f}, 26, 7.0f, 1.0f, 0.38f, 2);
                    for (int i = 0; i < 22; ++i) {
                        Vec3 d = rand_dir();
                        d.y = std::abs(d.y) + 0.7f;
                        emit(at + Vec3{0.0f, 0.2f, 0.0f}, d * frand(2.5f, 6.0f), Vec4{0.33f, 0.28f, 0.23f, 1.0f},
                             frand(0.6f, 1.0f), frand(0.08f, 0.16f), 0, 14.0f, 0.4f);
                    }
                    if (snd != nullptr) {
                        snd->play_at(SfxId::Thud, at, 1.0f, 0.75f);
                    }
                    if (glm::length(at - local_feet()) < kSlamRadius + 4.0f) {
                        cam_shake_ = std::max(cam_shake_, 1.1f);
                    }
                }
            }
        }
        // Elemental Shatter VFX: while chilled, drift a few frost motes; when the chill clears with the
        // enemy still alive (a heavy hit shattered it), burst icy shards.
        const bool chilled = (en.status & net::kStatusChilled) != 0u;
        if (chilled && dt.seconds > 0.0001f) {
            emit(en.position + Vec3{frand(-0.3f, 0.3f), frand(0.4f, 1.4f), frand(-0.3f, 0.3f)},
                 Vec3{0.0f, frand(-0.4f, 0.2f), 0.0f}, Vec4{0.7f, 0.88f, 1.0f, 0.8f}, 0.5f, 0.07f, 1);
        } else if (!chilled && (v.last_status & net::kStatusChilled) != 0u) {
            emit_burst(en.position + Vec3{0.0f, 0.9f, 0.0f}, Vec4{0.72f, 0.9f, 1.0f, 0.95f}, 20, 6.0f,
                       0.45f, 0.12f, 1, 1.0f, 3.0f);
            combat_text(en.position, "SHATTER!", Vec4{0.75f, 0.92f, 1.0f, 1.0f}); // the combo landed
            if (snd != nullptr) {
                snd->play_at(SfxId::Shatter, en.position, 0.9f);
            }
        }
        // The last raider standing snaps into a berserk fury: it roars, and the rage shows (see menace).
        if ((en.status & net::kStatusEnraged) != 0u && (v.last_status & net::kStatusEnraged) == 0u && !beast) {
            combat_text(en.position, "ENRAGED!", Vec4{1.0f, 0.38f, 0.22f, 1.0f});
            if (snd != nullptr) {
                snd->play_at(SfxId::Roar, en.position, 0.85f, 0.85f);
            }
        }
        // Bogged down in a dug pit: mud slops up round its legs as it wades.
        if ((en.status & net::kStatusMired) != 0u && v.speed > 0.3f && frand() < 0.4f) {
            emit(en.position + Vec3{frand(-0.3f, 0.3f), 0.15f, frand(-0.3f, 0.3f)},
                 Vec3{frand(-0.8f, 0.8f), frand(1.2f, 2.2f), frand(-0.8f, 0.8f)}, Vec4{0.24f, 0.18f, 0.12f, 1.0f}, 0.5f,
                 frand(0.06f, 0.1f), 0, 9.0f, 0.5f);
        }
        v.last_action = en.action;
        v.last_status = en.status;
        v.animator.update(v.speed, dt);
    }
    for (auto it = enemy_visuals_.begin(); it != enemy_visuals_.end();) {
        const bool live = std::any_of(snapshot_.enemies.begin(), snapshot_.enemies.end(),
                                      [&](const net::EnemyState& e) { return e.id == it->first; });
        if (live) {
            ++it;
            continue;
        }
        // A foe vanishing MID-FIGHT died (a haul's dusk/contract-end despawns flip the phase the same
        // tick, so those don't shower loot). Felled -> it pitches over and sinks away, spilling its purse
        // of golden coins; a sapper that was still healthy went up on its own satchel -> a fiery blast.
        EnemyVisual& dv = it->second;
        const bool fight = snapshot_.contract_phase == static_cast<u8>(ContractPhase::Active) ||
                           (dv.last_status & net::kStatusQuest) != 0u;
        if (fight && glm::length(dv.last_pos - local_feet()) < 80.0f) {
            const Vec3 at = dv.last_pos + Vec3{0.0f, 0.9f, 0.0f};
            Audio* s = audio();
            if (dv.kind == kEnemySapper && dv.last_health > 128u) {
                emit_burst(at, Vec4{1.0f, 0.55f, 0.2f, 1.0f}, 26, 7.0f, 0.5f, 0.16f, 1, 2.0f);
                emit_burst(at, Vec4{0.25f, 0.22f, 0.2f, 0.8f}, 14, 3.0f, 1.1f, 0.3f, 0, 2.5f);
                if (s != nullptr) {
                    s->play_at(SfxId::Explosion, at, 1.0f); // the satchel goes up
                }
            } else {
                for (int c = 0; c < (is_beast(dv.kind) ? 5 : 12); ++c) { // coins: golden glints tossed up, arcing down
                    emit(at, Vec3{frand(-2.2f, 2.2f), frand(2.5f, 5.5f), frand(-2.2f, 2.2f)},
                         Vec4{1.0f, 0.85f, 0.3f, 1.0f}, 0.9f, 0.09f, 1, 9.0f, 0.4f);
                }
                emit_burst(at, Vec4{0.5f, 0.42f, 0.35f, 0.7f}, 10, 2.5f, 0.6f, 0.2f, 0, 1.0f);
                if (s != nullptr) {
                    s->play_at(SfxId::Coin, at, 0.6f, frand(0.9f, 1.15f)); // the purse spills
                }
                // The body topples away from the blow (away from us, near enough) and sinks into the earth.
                EnemyDeath d;
                d.pos = dv.last_pos;
                d.yaw = std::atan2(dv.last_yaw_dir.z, dv.last_yaw_dir.x);
                d.scale = enemy_scale(dv.kind);
                Vec3 away = dv.last_pos - local_feet();
                away.y = 0.0f;
                d.fall = glm::length(away) > 0.1f ? glm::normalize(away) : -dv.last_yaw_dir;
                d.v = std::move(dv);
                if (enemy_deaths_.size() >= 12) {
                    retire_mesh(std::move(enemy_deaths_.front().v.body_mesh));
                    retire_mesh(std::move(enemy_deaths_.front().v.outfit_mesh));
                    enemy_deaths_.erase(enemy_deaths_.begin());
                }
                enemy_deaths_.push_back(std::move(d));
                it = enemy_visuals_.erase(it);
                continue;
            }
        }
        retire_mesh(std::move(dv.body_mesh)); // defer the GPU free past the frames in flight
        retire_mesh(std::move(dv.outfit_mesh));
        it = enemy_visuals_.erase(it);
    }
}

void ClientApp::draw_enemies() {
    if (!have_snapshot_) {
        return;
    }
    for (const net::EnemyState& en : snapshot_.enemies) {
        const auto it = enemy_visuals_.find(en.id);
        if (it == enemy_visuals_.end()) {
            continue;
        }
        EnemyVisual& v = it->second;
        // The tint: the kind's grimy cast, rimed blue when chilled, flushed red in a berserk rage, and
        // blanched pale for a beat when a blow lands.
        Vec3 tint = bandit_tint(en.kind);
        if ((en.status & net::kStatusChilled) != 0u) {
            tint = glm::mix(tint, Vec3{0.55f, 0.75f, 1.2f}, 0.6f); // chilled (Frost Bolt): an icy-blue rime
        }
        if ((en.status & net::kStatusEnraged) != 0u) {
            const f32 throb = 0.5f + 0.5f * std::sin(elapsed_ * 9.0f + static_cast<f32>(en.id));
            tint *= glm::mix(Vec3{1.0f}, Vec3{1.3f, 0.72f, 0.66f}, 0.4f + 0.3f * throb);
        }
        tint = glm::mix(tint, Vec3{1.8f, 1.55f, 1.45f}, v.hurt * 0.55f);
        if (is_beast(en.kind)) {
            draw_wolf(v, en, tint);
            continue;
        }
        // Bandit kinds: 0 grunt (dagger), 1 torch-bearer, 2 brute (maul, big), 3 archer (Outlaw kit +
        // bow), 4 shield-bearer (sword + big shield), 5 shaman (spirit fire), 6 sapper (satchel), 7 warlord.
        const f32 scale = enemy_scale(en.kind);
        const Mat4 root = glm::translate(Mat4{1.0f}, en.position) *
                          glm::rotate(Mat4{1.0f}, HalfPi - en.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                          glm::scale(Mat4{1.0f}, Vec3{scale}) * v.animator.body_offset() *
                          glm::rotate(Mat4{1.0f}, -0.22f * v.hurt, Vec3{1.0f, 0.0f, 0.0f}); // recoil
        const std::vector<Quat> pose = v.animator.pose(v.model);
        if (v.body_skin.vertices.empty()) {
            v.body_skin = build_body_mesh(v.model);
        }
        skin_and_draw(v.model, v.body_skin, v.body_mesh, root, pose, tint);     // continuous skinned body
        skin_and_draw(v.model, v.outfit_skin, v.outfit_mesh, root, pose, tint); // worn bandit leather
        const std::vector<Mat4> emats = v.model.bone_matrices(root, pose);
        draw_rig(v.model, emats, tint, /*attachments_only=*/true); // hood / mask / quiver / face on top
        // A crude weapon gripped in the weapon hand (the L-suffixed forearm on the mirrored rig), built
        // from the shared modular weapon_pieces at the tarnished bandit palette so it swings with the arm.
        const std::vector<Mat4> jmats = v.model.joint_matrices(root, pose);
        if (const WeaponType bw = bandit_weapon(en.kind); bw != WeaponType::None) {
            draw_weapon(bw, hand_frame(v.model, jmats, BonePart::LowerArmL) *
                                glm::mat4_cast(v.animator.weapon_wrist(v.model)),
                        v.model.palette(), EquipmentTier::Worn);
        }
        draw_enemy_menace(v, en, jmats, scale);
    }
}

// The ferocity layer over a raider's body: ember eyes, war paint and rank pieces bolted to its bones
// (they move with it), the brute's ground-splitting telegraph, the warlord's rallying cry, the berserk
// rage of the last one standing, and each kind's tools - torch, satchel, shaman's spirit fire, a skull-
// painted shield, a burning arrow nocked.
void ClientApp::draw_enemy_menace(EnemyVisual& v, const net::EnemyState& en, const std::vector<Mat4>& jmats, f32 scale) {
    const CharacterModel& m = v.model;
    const int hi = m.bone_index(BonePart::Head);
    if (hi < 0 || static_cast<usize>(hi) >= jmats.size()) {
        return;
    }
    const Bone& hb = m.bones()[static_cast<usize>(hi)];
    const f32 hs = hb.box_size.x;
    const f32 r = hs * 0.5f;
    const Vec3 hc = hb.box_center;
    const Mat4& head = jmats[static_cast<usize>(hi)];
    const Vec3 fwd{std::cos(en.yaw), 0.0f, std::sin(en.yaw)};
    const f32 night = 1.0f - sun_intensity_;
    const bool enraged = (en.status & net::kStatusEnraged) != 0u;
    const f32 rage = enraged ? 0.6f + 0.4f * std::sin(elapsed_ * 11.0f + static_cast<f32>(en.id)) : 0.0f;
    const Vec4 iron{0.2f, 0.19f, 0.2f, 1.0f};
    const Vec4 bone{0.84f, 0.8f, 0.68f, 1.0f};
    const Vec4 blood{0.3f, 0.035f, 0.03f, 1.0f};
    auto box_on = [&](const Mat4& frame, const Vec3& at, const Vec3& size, const Vec4& col, const Quat& rot = QuatIdentity) {
        renderer_->draw(shape_box_, frame * glm::translate(Mat4{1.0f}, at) * glm::mat4_cast(rot) * glm::scale(Mat4{1.0f}, size),
                        col);
    };
    auto roll = [](f32 a) { return glm::angleAxis(a, Vec3{0.0f, 0.0f, 1.0f}); };
    auto pitch = [](f32 a) { return glm::angleAxis(a, Vec3{1.0f, 0.0f, 0.0f}); };
    // A curving horn: tapering segments sweeping out from the temple, up, then forward to a point.
    auto horn = [&](f32 side, f32 size, const Vec4& col) {
        Vec3 p{hc.x + side * r * 0.75f, hc.y + r * 0.62f, hc.z - r * 0.1f};
        const Vec3 step[4] = {{side * 0.6f, 0.35f, 0.0f}, {side * 0.45f, 0.7f, 0.1f}, {side * 0.1f, 0.8f, 0.45f}, {0.0f, 0.4f, 0.85f}};
        f32 w = hs * 0.16f * size;
        for (int i = 0; i < 4; ++i) {
            const Vec3 d = glm::normalize(step[i]) * (hs * 0.18f * size);
            box_on(head, p + d * 0.5f, Vec3{w, glm::length(d) * 1.3f, w}, col,
                   from_to(Vec3{0.0f, 1.0f, 0.0f}, glm::normalize(d)));
            p += d;
            w *= 0.74f;
        }
    };
    // A short iron spike jutting from a joint frame.
    auto spike = [&](const Mat4& frame, const Vec3& at, const Vec3& dir, f32 len) {
        const Vec3 d = glm::normalize(dir);
        box_on(frame, at + d * len * 0.5f, Vec3{len * 0.22f, len, len * 0.22f}, iron, from_to(Vec3{0.0f, 1.0f, 0.0f}, d));
    };

    // EMBER EYES: the glowing eyes (the eye bones are emissive) get a glint that blooms in the dark -
    // and flares when it's enraged.
    for (const Vec3& e : eye_points(m, jmats)) {
        const Vec3 toward = glm::normalize(camera_.position() - e) * 0.02f;
        renderer_->draw_sprite(e + toward, (0.04f + 0.025f * night + 0.03f * rage) * scale,
                               Vec4{1.0f, 0.42f + 0.2f * rage, 0.12f, 0.55f + 0.35f * night + 0.4f * rage}, 0.75f);
    }
    // WAR PAINT: a band of dried blood smeared across the eyes (not the archers - their scarves ride high).
    if (en.kind != 3u && en.kind != kEnemyHealer) {
        box_on(head, Vec3{hc.x, hc.y + r * 0.1f, hc.z + r * 0.995f}, Vec3{hs * 1.04f, hs * 0.12f, 0.012f}, blood);
    }

    const int iul = m.bone_index(BonePart::UpperArmL), iur = m.bone_index(BonePart::UpperArmR);
    const int it = m.bone_index(BonePart::Torso);
    auto shoulder_spikes = [&](int n, f32 len) {
        for (const int ia : {iul, iur}) {
            if (ia < 0) {
                continue;
            }
            const f32 o = m.bones()[static_cast<usize>(ia)].joint_offset.x < 0.0f ? -1.0f : 1.0f;
            for (int k = 0; k < n; ++k) {
                spike(jmats[static_cast<usize>(ia)], Vec3{o * 0.05f, 0.08f, -0.05f + 0.07f * static_cast<f32>(k)},
                      Vec3{o * 0.7f, 1.0f, 0.0f}, len);
            }
        }
    };
    switch (en.kind) {
        case 2u: { // BRUTE: curling horns, tusks jutting from the jaw, spiked iron shoulders
            horn(-1.0f, 1.2f, bone);
            horn(1.0f, 1.2f, bone);
            for (const f32 s : {-1.0f, 1.0f}) {
                box_on(head, Vec3{hc.x + s * r * 0.45f, hc.y - r * 0.55f, hc.z + r * 0.95f}, Vec3{0.03f, 0.1f, 0.03f}, bone,
                       pitch(-0.35f) * roll(s * 0.25f));
            }
            shoulder_spikes(3, 0.16f);
            break;
        }
        case kEnemyWarlord: { // WARLORD: a horned skull helm, a crest of spikes, a war banner at the back
            // A great beast's skull worn as a helm: the rounded cranium over the crown, a heavy brow
            // with dark eye sockets glaring out, cheek bones down the sides of the face.
            const Vec4 skull{0.7f, 0.66f, 0.56f, 1.0f};
            renderer_->draw(shape_rounded_,
                            head * glm::translate(Mat4{1.0f}, Vec3{hc.x, hc.y + r * 0.62f, hc.z - r * 0.05f}) *
                                glm::scale(Mat4{1.0f}, Vec3{hs * 1.08f, hs * 0.5f, hs * 1.1f}),
                            skull);
            box_on(head, Vec3{hc.x, hc.y + r * 0.34f, hc.z + r * 0.98f}, Vec3{hs * 1.0f, hs * 0.16f, hs * 0.12f}, skull);
            for (const f32 s : {-1.0f, 1.0f}) {
                box_on(head, Vec3{hc.x + s * r * 0.4f, hc.y + r * 0.34f, hc.z + r * 1.05f}, Vec3{hs * 0.2f, hs * 0.1f, 0.02f},
                       Vec4{0.05f, 0.03f, 0.03f, 1.0f}); // the sockets
                box_on(head, Vec3{hc.x + s * r * 0.95f, hc.y - r * 0.1f, hc.z + r * 0.45f}, Vec3{0.03f, hs * 0.55f, hs * 0.35f}, skull,
                       roll(s * 0.12f)); // cheek guards
            }
            horn(-1.0f, 1.6f, Vec4{0.14f, 0.12f, 0.12f, 1.0f});
            horn(1.0f, 1.6f, Vec4{0.14f, 0.12f, 0.12f, 1.0f});
            for (int k = 0; k < 4; ++k) {
                spike(head, Vec3{hc.x, hc.y + r * 0.95f, hc.z + r * (0.5f - 0.35f * static_cast<f32>(k))}, Vec3{0.0f, 1.0f, -0.25f},
                      0.12f);
            }
            shoulder_spikes(2, 0.18f);
            if (it >= 0) { // the banner pole riding the back, its crimson flag snapping in the wind
                const Mat4& torso = jmats[static_cast<usize>(it)];
                box_on(torso, Vec3{0.0f, 0.55f, -0.2f}, Vec3{0.05f, 1.9f, 0.05f}, Vec4{0.15f, 0.1f, 0.08f, 1.0f});
                const f32 flap = 0.25f * std::sin(elapsed_ * 4.0f + static_cast<f32>(en.id));
                box_on(torso, Vec3{0.0f, 1.25f, -0.42f}, Vec3{0.03f, 0.55f, 0.42f}, Vec4{0.7f, 0.08f, 0.1f, 1.0f},
                       glm::angleAxis(flap, Vec3{0.0f, 1.0f, 0.0f}));
                box_on(torso, Vec3{0.0f, 1.32f, -0.42f}, Vec3{0.035f, 0.14f, 0.14f}, bone); // a skull sigil on it
            }
            // The war-cry: every few seconds a ripple of red dust rolls out across its rally zone.
            const f32 period = 2.8f;
            if (std::fmod(elapsed_ + static_cast<f32>(en.id) * 0.37f, period) < frame_dt_) {
                emit_ring(en.position, Vec4{0.75f, 0.16f, 0.12f, 0.6f}, 36, kWarlordAuraRadius * 1.7f, 0.6f, 0.2f, 1);
            }
            break;
        }
        case 0u: { // GRUNT: one in three wears a horned iron cap, one a spiked collar
            if (en.id % 3u == 0u) {
                box_on(head, Vec3{hc.x, hc.y + r * 0.72f, hc.z}, Vec3{hs * 1.08f, hs * 0.32f, hs * 1.08f}, iron);
                horn(-1.0f, 0.65f, bone);
                horn(1.0f, 0.65f, bone);
            } else if (en.id % 3u == 1u && it >= 0) {
                const Mat4& torso = jmats[static_cast<usize>(it)];
                for (int k = 0; k < 5; ++k) {
                    const f32 a = -1.2f + 0.6f * static_cast<f32>(k);
                    spike(torso, Vec3{std::sin(a) * 0.17f, 0.52f, std::cos(a) * 0.14f}, Vec3{std::sin(a), 0.4f, std::cos(a)}, 0.1f);
                }
            } else {
                shoulder_spikes(1, 0.13f);
            }
            break;
        }
        case kEnemyShield: { // SHIELD-BEARER: a round shield on the off arm, a skull daubed on it in white
            const Mat4 hand = hand_frame(m, jmats, BonePart::LowerArmR);
            const Mat4 face = hand * glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.05f, 0.12f}) *
                              glm::rotate(Mat4{1.0f}, HalfPi, Vec3{1.0f, 0.0f, 0.0f});
            renderer_->draw(shape_cylinder_, face * glm::scale(Mat4{1.0f}, Vec3{0.62f, 0.06f, 0.62f}), Vec4{0.38f, 0.25f, 0.14f, 1.0f});
            renderer_->draw(shape_cylinder_, face * glm::scale(Mat4{1.0f}, Vec3{0.66f, 0.04f, 0.66f}), iron);
            const Mat4 paint = face * glm::translate(Mat4{1.0f}, Vec3{0.0f, -0.04f, 0.0f});
            box_on(paint, Vec3{0.0f, 0.0f, 0.02f}, Vec3{0.22f, 0.01f, 0.22f}, bone);
            for (const f32 s : {-1.0f, 1.0f}) {
                box_on(paint, Vec3{s * 0.055f, -0.008f, -0.01f}, Vec3{0.06f, 0.01f, 0.06f}, Vec4{0.05f, 0.04f, 0.04f, 1.0f});
            }
            spike(face, Vec3{0.0f, -0.05f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 0.16f); // the boss spike
            break;
        }
        case kEnemyHealer: { // SHAMAN: an antler headdress and a gnarled staff crowned with sickly spirit fire
            for (const f32 s : {-1.0f, 1.0f}) {
                box_on(head, Vec3{hc.x + s * r * 0.5f, hc.y + r * 1.1f, hc.z}, Vec3{0.03f, hs * 0.7f, 0.03f}, bone, roll(-s * 0.35f));
                box_on(head, Vec3{hc.x + s * r * 0.95f, hc.y + r * 1.35f, hc.z}, Vec3{0.025f, hs * 0.35f, 0.025f}, bone, roll(-s * 0.9f));
            }
            const Mat4 hand = hand_frame(m, jmats, BonePart::LowerArmL);
            const Vec3 grip = Vec3{hand[3]};
            const Vec3 top = grip + Vec3{0.0f, 0.75f * scale, 0.0f};
            const Vec3 foot{grip.x, en.position.y, grip.z};
            renderer_->draw(shape_box_,
                            glm::translate(Mat4{1.0f}, (top + foot) * 0.5f) * orient_to(top - foot) *
                                glm::scale(Mat4{1.0f}, Vec3{0.05f, 0.05f, glm::length(top - foot)}),
                            Vec4{0.26f, 0.19f, 0.12f, 1.0f});
            renderer_->draw(shape_sphere_, glm::translate(Mat4{1.0f}, top) * glm::scale(Mat4{1.0f}, Vec3{0.14f}), bone); // a skull
            const f32 flick = 0.8f + 0.2f * std::sin(elapsed_ * 17.0f + static_cast<f32>(en.id));
            const Vec3 fire = top + Vec3{0.0f, 0.16f, 0.0f};
            renderer_->draw_sprite(fire, 0.22f * flick, Vec4{0.45f, 1.0f, 0.4f, 0.75f}, 0.3f);
            if (frand() < 0.8f) {
                emit_ember(fire + rand_dir() * 0.06f, Vec3{frand(-0.2f, 0.2f), frand(0.9f, 1.6f), frand(-0.2f, 0.2f)},
                           Vec3{0.75f, 1.0f, 0.55f}, Vec3{0.1f, 0.45f, 0.15f}, frand(0.35f, 0.6f), frand(0.06f, 0.11f), -0.6f, 0.85f);
            }
            fx_light(fire, Vec3{0.4f, 1.0f, 0.45f}, 1.6f * flick, 5.0f);
            break;
        }
        case 1u: { // TORCH-BEARER: a pitch torch in hand, flames licking up off it
            const Mat4 hand = hand_frame(m, jmats, BonePart::LowerArmL);
            const Vec3 grip = Vec3{hand[3]};
            const Vec3 tip = grip + Vec3{0.0f, 0.5f, 0.0f} + fwd * 0.12f;
            renderer_->draw(shape_box_,
                            glm::translate(Mat4{1.0f}, (grip + tip) * 0.5f - Vec3{0.0f, 0.1f, 0.0f}) * orient_to(tip - grip) *
                                glm::scale(Mat4{1.0f}, Vec3{0.06f, 0.06f, 0.75f}),
                            Vec4{0.32f, 0.2f, 0.1f, 1.0f});
            const f32 flick = 0.85f + 0.15f * std::sin(elapsed_ * 13.0f + en.position.x);
            renderer_->draw_sprite(tip, 0.32f * flick, Vec4{1.0f, 0.55f, 0.18f, 0.7f}, 0.25f);
            renderer_->draw_sprite(tip, 0.12f, Vec4{1.0f, 0.9f, 0.6f, 1.0f}, 0.8f);
            for (int k = 0; k < 2; ++k) {
                emit_ember(tip + rand_dir() * 0.07f, Vec3{frand(-0.3f, 0.3f), frand(1.4f, 2.4f), frand(-0.3f, 0.3f)},
                           Vec3{1.0f, 0.85f, 0.4f}, Vec3{0.75f, 0.15f, 0.04f}, frand(0.3f, 0.55f), frand(0.08f, 0.15f), -1.0f);
            }
            Renderer::SpotLight sl;
            sl.position = tip + Vec3{0.0f, 1.5f, 0.0f};
            sl.direction = Vec3{0.0f, -1.0f, 0.0f};
            sl.color = Vec3{1.0f, 0.55f, 0.22f} * (2.4f * flick);
            sl.range = 8.0f;
            sl.cone_outer_cos = std::cos(glm::radians(70.0f));
            sl.cone_inner_cos = std::cos(glm::radians(35.0f));
            renderer_->add_light(sl);
            break;
        }
        case kEnemySapper: { // SAPPER: a lumpy satchel charge strapped to its back, the fuse spitting sparks
            if (it >= 0) {
                const Mat4& torso = jmats[static_cast<usize>(it)];
                box_on(torso, Vec3{0.0f, 0.32f, -0.22f}, Vec3{0.3f, 0.32f, 0.2f}, Vec4{0.24f, 0.19f, 0.13f, 1.0f});
                box_on(torso, Vec3{0.0f, 0.32f, -0.33f}, Vec3{0.32f, 0.05f, 0.03f}, iron); // a strap
                const Vec3 fuse = Vec3{torso * Vec4{0.08f, 0.56f, -0.22f, 1.0f}};
                const f32 spark = 0.6f + 0.4f * std::sin(elapsed_ * 26.0f + en.position.x);
                renderer_->draw_sprite(fuse, 0.12f * spark + 0.05f, Vec4{1.0f, 0.75f, 0.3f, 1.0f}, 0.7f);
                emit(fuse, rand_dir() * frand(1.0f, 2.5f) + Vec3{0.0f, 1.0f, 0.0f}, Vec4{1.0f, 0.8f, 0.35f, 1.0f}, 0.25f, 0.04f, 1,
                     6.0f);
                fx_light(fuse, Vec3{1.0f, 0.6f, 0.2f}, 1.2f * spark, 4.0f);
            }
            break;
        }
        case 3u: { // ARCHER: a burning arrow nocked while it draws a bead
            if (en.action == 2) {
                const Vec3 hand = Vec3{hand_frame(m, jmats, BonePart::LowerArmL)[3]};
                const Vec3 tip = hand + fwd * 0.35f;
                renderer_->draw_sprite(tip, 0.13f, Vec4{1.0f, 0.55f, 0.2f, 0.85f}, 0.5f);
                emit_ember(tip, Vec3{0.0f, frand(0.6f, 1.2f), 0.0f}, Vec3{1.0f, 0.8f, 0.4f}, Vec3{0.7f, 0.15f, 0.05f}, 0.35f,
                           0.07f, -0.8f);
                fx_light(tip, Vec3{1.0f, 0.55f, 0.2f}, 1.4f, 4.0f);
            }
            break;
        }
        default: break;
    }

    // The BRUTE's slam winding up: fissures of glowing heat race out through the ground to the edge of
    // the blast and dust jumps along its rim - dodge clear before the maul comes down.
    if (en.kind == 2u && en.action == 2) {
        const Vec3 c{en.position.x, en.position.y + 0.06f, en.position.z};
        const f32 puls = 0.55f + 0.45f * std::sin(elapsed_ * 14.0f);
        for (int k = 0; k < 9; ++k) {
            const f32 a = TwoPi * static_cast<f32>(k) / 9.0f + static_cast<f32>(en.id);
            const Vec3 d{std::cos(a), 0.0f, std::sin(a)};
            const Vec3 bend{-d.z, 0.0f, d.x};
            const Vec3 mid = c + d * (kSlamRadius * 0.5f) + bend * 0.25f * std::sin(a * 3.0f);
            const Vec4 heat{1.0f, 0.42f, 0.14f, (0.3f + 0.3f * night) * puls};
            renderer_->draw_sprite(c + d * 0.4f, mid, 0.05f, heat, 0.4f);
            renderer_->draw_sprite(mid, c + d * kSlamRadius, 0.04f, heat, 0.4f);
        }
        sprite_circle(c, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, kSlamRadius, 44, 0.05f,
                      Vec4{1.0f, 0.4f, 0.14f, 0.5f * puls}, 0.7f);
        if (frand() < 0.6f) {
            const f32 a = frand(0.0f, TwoPi);
            emit(c + Vec3{std::cos(a), 0.0f, std::sin(a)} * kSlamRadius, Vec3{0.0f, frand(0.8f, 1.8f), 0.0f},
                 Vec4{0.55f, 0.48f, 0.4f, 0.45f}, 0.6f, frand(0.12f, 0.2f), 2, 3.0f);
        }
    }

    // The berserk RAGE of the last one standing: embers boiling off its shoulders, hot breath steaming.
    if (enraged) {
        const Vec3 chest = en.position + Vec3{0.0f, 1.2f * scale, 0.0f};
        for (int k = 0; k < 2; ++k) {
            emit_ember(chest + Vec3{frand(-0.35f, 0.35f), frand(0.0f, 0.4f), frand(-0.35f, 0.35f)},
                       Vec3{frand(-0.3f, 0.3f), frand(1.2f, 2.4f), frand(-0.3f, 0.3f)}, Vec3{1.0f, 0.55f, 0.2f},
                       Vec3{0.55f, 0.05f, 0.02f}, frand(0.35f, 0.6f), frand(0.06f, 0.12f), -0.8f);
        }
        if (frand() < 0.25f) {
            const Vec3 mouth = Vec3{head * Vec4{hc.x, hc.y - r * 0.3f, hc.z + r * 1.2f, 1.0f}};
            emit(mouth, fwd * 0.8f + Vec3{0.0f, 0.5f, 0.0f}, Vec4{0.8f, 0.78f, 0.76f, 0.35f}, 0.6f, 0.12f, 0, -0.3f);
        }
        fx_light(chest, Vec3{1.0f, 0.3f, 0.12f}, 1.2f * rage, 4.0f);
    }
}

void ClientApp::draw_wolf(EnemyVisual& v, const net::EnemyState& en, const Vec3& tint, const Mat4* pose_root) {
    const bool alpha = en.kind == kEnemyAlpha;
    const f32 s = enemy_scale(en.kind);
    const f32 run = glm::clamp(v.speed / kWolfSpeed, 0.0f, 1.3f);
    const bool crouch = en.action == 2;
    const bool pounce = en.action == 4;
    // The body: a loping bob at a run, crouched low + nose down coiled to spring, stretched out + lifted
    // mid-pounce.
    Mat4 base;
    if (pose_root != nullptr) {
        base = *pose_root;
    } else {
        const f32 bob = run * 0.07f * std::abs(std::sin(v.gait));
        const f32 lift = pounce ? 0.35f : 0.0f;
        const f32 sink = crouch ? 0.14f : 0.0f;
        const f32 pitch = crouch ? -0.1f : pounce ? 0.16f : 0.04f * std::sin(v.gait * 2.0f) * run;
        base = glm::translate(Mat4{1.0f}, en.position + Vec3{0.0f, bob + lift - sink, 0.0f}) *
               glm::rotate(Mat4{1.0f}, -en.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
               glm::rotate(Mat4{1.0f}, pitch, Vec3{0.0f, 0.0f, 1.0f}) * glm::scale(Mat4{1.0f}, Vec3{s});
    }
    const Vec4 fur{tint * (alpha ? 0.62f : 1.0f), 1.0f}; // the alpha's coat is near-black
    renderer_->draw(wolf_body_mesh_, base, fur);
    renderer_->draw(wolf_jaw_mesh_, base * glm::translate(Mat4{1.0f}, kWolfJaw) *
                                        glm::rotate(Mat4{1.0f}, -0.6f * v.jaw, Vec3{0.0f, 0.0f, 1.0f}),
                    fur);
    // The legs: a bounding gallop (the fore pair together, the hind pair together), tucked under and
    // braced when crouched, flung fore + aft mid-pounce.
    for (int k = 0; k < 4; ++k) {
        const bool front = k < 2;
        f32 swing = std::sin(v.gait + (front ? 0.0f : Pi * 0.85f) + (k % 2 == 0 ? 0.0f : 0.35f)) * (0.2f + 0.55f * std::min(run, 1.0f));
        if (crouch) {
            swing = front ? 0.45f : -0.55f;
        } else if (pounce) {
            swing = front ? 1.15f : -1.05f;
        }
        renderer_->draw(wolf_leg_mesh_,
                        base * glm::translate(Mat4{1.0f}, kWolfLegs[k]) * glm::rotate(Mat4{1.0f}, swing, Vec3{0.0f, 0.0f, 1.0f}),
                        fur);
    }
    if (pose_root != nullptr) {
        return; // a fallen wolf's eyes have gone dark
    }
    // Eyes burning out of the dark - amber, hotter on the alpha - blooming at night.
    const f32 night = 1.0f - sun_intensity_;
    for (const Vec3& e : kWolfEyes) {
        const Vec3 p = Vec3{base * Vec4{e + Vec3{0.03f, 0.0f, 0.0f}, 1.0f}};
        renderer_->draw_sprite(p, (0.045f + 0.03f * night) * s, Vec4{1.0f, alpha ? 0.45f : 0.78f, 0.15f, 0.8f + 0.4f * night},
                               0.8f);
    }
    // Hot breath steaming from the snarl when it crouches.
    if (crouch && frand() < 0.4f) {
        const Vec3 mouth = Vec3{base * Vec4{1.1f, 0.85f, 0.0f, 1.0f}};
        emit(mouth, Vec3{std::cos(en.yaw), 0.4f, std::sin(en.yaw)} * 0.8f, Vec4{0.85f, 0.85f, 0.85f, 0.3f}, 0.5f, 0.1f, 0, -0.2f);
    }
    // Torn-up earth kicked out behind a pounce.
    if (pounce && frand() < 0.5f) {
        emit(en.position + Vec3{0.0f, 0.1f, 0.0f}, Vec3{-std::cos(en.yaw), 1.2f, -std::sin(en.yaw)} * 1.5f,
             Vec4{0.32f, 0.26f, 0.2f, 1.0f}, 0.5f, 0.07f, 0, 9.0f);
    }
}

void ClientApp::draw_enemy_deaths(Timestep dt) {
    // Bury the long-gone first: erasing shifts the rest down the vector, which must not happen after
    // their meshes were submitted this frame (the renderer holds pointers to them until it records).
    for (auto it = enemy_deaths_.begin(); it != enemy_deaths_.end();) {
        if (it->t > 2.8f) {
            retire_mesh(std::move(it->v.body_mesh));
            retire_mesh(std::move(it->v.outfit_mesh));
            it = enemy_deaths_.erase(it);
        } else {
            ++it;
        }
    }
    for (EnemyDeath& d : enemy_deaths_) {
        d.t += dt.seconds;
        // It pitches over (a heavy body falls faster at the end), lies a moment, then sinks into the earth.
        const f32 tip = glm::smoothstep(0.0f, 0.55f, d.t);
        const f32 fall = tip * tip * 1.5f;
        const f32 sink = glm::smoothstep(1.4f, 2.6f, d.t) * 1.4f * d.scale;
        const Vec3 axis = glm::normalize(glm::cross(Vec3{0.0f, 1.0f, 0.0f}, d.fall));
        const Mat4 topple = glm::translate(Mat4{1.0f}, d.pos - Vec3{0.0f, sink, 0.0f}) * glm::rotate(Mat4{1.0f}, fall, axis);
        if (is_beast(d.v.kind)) {
            net::EnemyState es;
            es.kind = d.v.kind;
            es.position = d.pos;
            es.yaw = d.yaw;
            const Mat4 root = topple * glm::rotate(Mat4{1.0f}, -d.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                              glm::scale(Mat4{1.0f}, Vec3{d.scale});
            draw_wolf(d.v, es, Vec3{0.85f}, &root);
            continue;
        }
        const Mat4 root = topple * glm::rotate(Mat4{1.0f}, HalfPi - d.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                          glm::scale(Mat4{1.0f}, Vec3{d.scale});
        const std::vector<Quat> pose = d.v.animator.pose(d.v.model);
        const Vec3 tint = bandit_tint(d.v.kind) * 0.85f;
        skin_and_draw(d.v.model, d.v.body_skin, d.v.body_mesh, root, pose, tint);
        skin_and_draw(d.v.model, d.v.outfit_skin, d.v.outfit_mesh, root, pose, tint);
        draw_rig(d.v.model, d.v.model.bone_matrices(root, pose), tint, /*attachments_only=*/true);
        // A last puff of dust where it hits the ground.
        if (d.t > 0.5f && d.t - dt.seconds <= 0.5f) {
            emit_burst(d.pos + d.fall * 0.8f + Vec3{0.0f, 0.15f, 0.0f}, Vec4{0.55f, 0.48f, 0.4f, 0.4f}, 8, 1.8f, 0.9f, 0.24f, 2,
                       0.6f);
        }
    }
}

// Rebuilds the list of town gates near the player and eases each toward open (1) when a
// networked player / villager / enemy is close, else shut (0). Client-side + visual only.
void ClientApp::update_gates(Timestep dt) {
    gates_.clear();
    if (!have_snapshot_ || world_seed_ == 0) {
        return;
    }
    const Vec3 feet = local_feet();
    const f32 ease = glm::clamp(dt.seconds * 4.0f, 0.0f, 1.0f);
    constexpr f32 kOpenRange = 8.0f; // doors begin opening within this of the gate
    const int vcx = static_cast<int>(std::floor(feet.x / worldgen::village_cell));
    const int vcz = static_cast<int>(std::floor(feet.z / worldgen::village_cell));
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const auto v = worldgen::village_at(vcx + dx, vcz + dz, world_seed_);
            if (!v) {
                continue;
            }
            for (const detail::VillageGate& g : detail::village_gate_points(*v, world_seed_)) {
                if (glm::length(Vec2{g.pos.x - feet.x, g.pos.y - feet.z}) > 95.0f) {
                    continue; // only animate gates near the player
                }
                f32 nd = 1e9f;
                auto consider = [&](const Vec3& p) {
                    nd = std::min(nd, glm::length(Vec2{p.x - g.pos.x, p.z - g.pos.y}));
                };
                for (const net::PlayerState& p : snapshot_.players) consider(p.position);
                for (const net::VillagerState& vl : snapshot_.villagers) consider(vl.position);
                for (const net::EnemyState& en : snapshot_.enemies) consider(en.position);
                const f32 target = glm::smoothstep(kOpenRange, kOpenRange * 0.55f, nd);
                const u64 id = (static_cast<u64>(static_cast<u32>(static_cast<i32>(std::lround(g.pos.x)))) << 32) |
                               static_cast<u64>(static_cast<u32>(static_cast<i32>(std::lround(g.pos.y))));
                f32& o = gate_open_[id];
                o = glm::mix(o, target, ease);
                const f32 gy = worldgen::height(g.pos.x, g.pos.y, world_seed_);
                gates_.push_back(GateVisual{Vec3{g.pos.x, gy, g.pos.y},
                                            glm::normalize(g.pos - v->center), g.half, o});
            }
        }
    }
}

// Draws each nearby gate as two planked door leaves, hinged at the sides of the opening and
// swinging outward (from across the gap when shut to pointing along the radial when open).
void ClientApp::draw_gates() {
    if (renderer_ == nullptr) {
        return;
    }
    constexpr f32 doorH = 2.6f; // a touch taller than the wall
    const Vec4 wood{0.46f, 0.32f, 0.18f, 1.0f};
    for (const GateVisual& g : gates_) {
        const f32 leaf_w = g.half; // each leaf spans the opening half-width (wider for multi-road gates)
        const Vec2 rad = g.radial;
        const Vec2 tang{-rad.y, rad.x}; // along the wall, across the opening
        const Vec2 gxz{g.pos.x, g.pos.z};
        const f32 open_yaw = std::atan2(-rad.y, rad.x); // fully open: leaf points outward
        for (f32 side : {-1.0f, 1.0f}) {
            const Vec2 hinge = gxz + tang * (side * leaf_w);
            const Vec2 closed = -side * tang; // hinge -> opening centre when shut
            const f32 base_yaw = std::atan2(-closed.y, closed.x);
            f32 delta = open_yaw - base_yaw; // shortest path closed -> open (~90 deg)
            while (delta > Pi) delta -= TwoPi;
            while (delta < -Pi) delta += TwoPi;
            const f32 yaw = base_yaw + delta * g.open;
            const Mat4 m = glm::translate(Mat4{1.0f}, Vec3{hinge.x, g.pos.y, hinge.y}) *
                           glm::rotate(Mat4{1.0f}, yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                           glm::scale(Mat4{1.0f}, Vec3{leaf_w, doorH, 1.0f});
            renderer_->draw(gate_door_mesh_, m, wood);
        }
    }
}

void ClientApp::draw_bridges() {
    if (renderer_ == nullptr || world_seed_ == 0) {
        return;
    }
    const Vec3 feet = local_feet();
    for (const roads::Bridge& b : roads::bridges(Vec2{feet.x, feet.z}, 100.0f, world_seed_)) {
        // The deck meets each bank flush (no lip): position at the mid-bank height and PITCH the span
        // so the back end sits at bank_a and the front end at bank_b; the mesh adds the arch hump. This
        // matches roads::bridge_deck_y exactly, so the visible deck is the walkable deck.
        const f32 base_y = (b.bank_a + b.bank_b) * 0.5f;
        const f32 pitch = std::asin(glm::clamp((b.bank_b - b.bank_a) / b.length, -0.6f, 0.6f));
        const Mat4 m = glm::translate(Mat4{1.0f}, Vec3{b.center.x, base_y, b.center.y}) *
                       glm::rotate(Mat4{1.0f}, -b.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                       glm::rotate(Mat4{1.0f}, pitch, Vec3{0.0f, 0.0f, 1.0f}) *
                       glm::scale(Mat4{1.0f}, Vec3{b.length, 1.0f, 1.0f});
        renderer_->draw(b.kind == 1 ? bridge_mesh_wood_ : bridge_mesh_stone_, m, Vec4{1.0f});
    }
}

void ClientApp::draw_barricades() {
    if (!have_snapshot_) {
        return;
    }
    for (const net::BarricadeState& b : snapshot_.barricades) {
        const f32 hp = static_cast<f32>(b.health) / 255.0f;
        const Vec3 wood = glm::mix(Vec3{0.22f, 0.14f, 0.08f}, Vec3{0.46f, 0.32f, 0.19f}, hp);
        const Vec4 c{wood, 1.0f};
        const Mat4 m = glm::translate(Mat4{1.0f}, b.position) *
                       glm::rotate(Mat4{1.0f}, b.yaw, Vec3{0.0f, 1.0f, 0.0f});
        auto bar = [&](const Vec3& off, const Vec3& scale) {
            renderer_->draw(shape_box_, m * glm::translate(Mat4{1.0f}, off) *
                                            glm::scale(Mat4{1.0f}, scale),
                            c);
        };
        bar({0.0f, 0.5f, 0.0f}, {2.0f, 0.14f, 0.12f});  // lower rail
        bar({0.0f, 0.9f, 0.0f}, {2.0f, 0.14f, 0.12f});  // upper rail
        for (int i = -2; i <= 2; ++i) {
            const f32 x = static_cast<f32>(i) * 0.45f;
            bar({x, 0.6f, 0.0f}, {0.13f, 1.25f, 0.13f}); // upright stakes
        }
    }
}

void ClientApp::draw_fires() {
    if (renderer_ == nullptr || !have_snapshot_) {
        return;
    }
    // A cheap deterministic [0,1) hash for per-tongue variation.
    auto frac = [](f32 x) { return x - std::floor(x); };
    const f32 night = 1.0f - sun_intensity_;
    for (const net::FireState& fs : snapshot_.fires) {
        const f32 in = static_cast<f32>(fs.intensity) / 255.0f;
        const f32 ember = in < 0.35f ? 1.0f : 0.0f; // burnt-out ruin?
        const f32 vigour = glm::clamp(in, 0.25f, 1.0f);
        const f32 ph = fs.position.x * 1.3f + fs.position.z * 0.7f; // per-fire phase
        const Vec3 base = fs.position;

        // Flame tongues spread over the ~6x5.5m footprint, rising toward the roof.
        const int tongues = ember > 0.0f ? 5 : 13;
        for (int i = 0; i < tongues; ++i) {
            const f32 fi = static_cast<f32>(i);
            const f32 a = frac(fi * 0.61f) * TwoPi + ph;
            const f32 spread = ember > 0.0f ? 1.0f : 2.0f;
            const f32 ox = std::cos(a) * (0.3f + frac(fi * 0.37f) * spread);
            const f32 oz = std::sin(a) * (0.3f + frac(fi * 0.53f) * spread * 0.9f);
            const f32 flick = std::sin(elapsed_ * (5.0f + fi * 0.7f) + ph + fi);
            const f32 grow = ember > 0.0f ? 0.55f : (1.4f + frac(fi * 0.29f) * 1.9f);
            const f32 height = grow * vigour * (0.82f + 0.18f * flick);
            const f32 width = (0.32f + frac(fi * 0.41f) * 0.32f) * vigour;
            const Vec3 col = glm::mix(Vec3{1.0f, 0.42f, 0.08f}, Vec3{1.0f, 0.88f, 0.35f},
                                      frac(fi * 0.71f) * 0.7f);
            const Mat4 m = glm::translate(Mat4{1.0f}, base + Vec3{ox + 0.12f * flick,
                                                                  height * 0.5f, oz}) *
                           glm::scale(Mat4{1.0f}, Vec3{width, height, width});
            renderer_->draw_emissive(shape_sphere_, m, Vec4{col, 1.0f});
        }

        // Smoke: dark puffs rising and fattening as they climb.
        const int puffs = 4;
        for (int i = 0; i < puffs; ++i) {
            const f32 t = frac(elapsed_ * 0.16f + static_cast<f32>(i) / puffs + ph);
            const f32 yy = 3.2f + t * 5.0f;
            const f32 ss = (0.9f + t * 2.2f) * (0.6f + 0.4f * vigour);
            const f32 alpha = (1.0f - t) * (ember > 0.0f ? 0.5f : 0.38f);
            const Mat4 m = glm::translate(Mat4{1.0f},
                                          base + Vec3{0.7f * std::sin(elapsed_ * 0.6f + i),
                                                      yy, 0.5f * std::cos(elapsed_ * 0.5f + i)}) *
                           glm::scale(Mat4{1.0f}, Vec3{ss});
            renderer_->draw_transparent(shape_sphere_, m, Vec4{0.09f, 0.08f, 0.08f, alpha});
        }

        // Additive firelight bloom.
        const f32 bloom = 0.6f * vigour * (0.85f + 0.15f * std::sin(elapsed_ * 9.0f + ph));
        renderer_->draw_glow(shape_sphere_,
                             glm::translate(Mat4{1.0f}, base + Vec3{0.0f, 1.6f, 0.0f}) *
                                 glm::scale(Mat4{1.0f}, Vec3{3.6f * vigour}),
                             Vec4{1.0f, 0.5f, 0.18f, bloom});

        // A strong warm light pooling around the blaze (brightest at night).
        if (in > 0.15f) {
            Renderer::SpotLight sl;
            sl.position = base + Vec3{0.0f, 4.5f, 0.0f};
            sl.direction = Vec3{0.0f, -1.0f, 0.0f};
            sl.color = Vec3{1.0f, 0.5f, 0.2f} * ((0.6f + 1.4f * night) * vigour);
            sl.range = 15.0f;
            const f32 half = glm::radians(62.0f);
            sl.cone_outer_cos = std::cos(half);
            sl.cone_inner_cos = std::cos(half * 0.6f);
            renderer_->add_light(sl);
        }
    }
}

f32 ClientApp::house_burn(const Vec3& p) const {
    if (!have_snapshot_) {
        return 0.0f;
    }
    f32 burn = 0.0f;
    for (const net::FireState& fs : snapshot_.fires) {
        if (glm::length(Vec2{fs.position.x - p.x, fs.position.z - p.z}) < 2.5f) {
            burn = std::max(burn, static_cast<f32>(fs.intensity) / 255.0f);
        }
    }
    return burn;
}

void ClientApp::draw_prop(const PropInstance& p) {
    const std::vector<GpuProp>& set =
        p.category == PropCategory::Bush      ? gpu_bushes_
        : p.category == PropCategory::Rock    ? gpu_rocks_
        : p.category == PropCategory::Log     ? gpu_logs_
        : p.category == PropCategory::Fence   ? gpu_fences_
        : p.category == PropCategory::FenceRail ? gpu_fence_rails_
        : p.category == PropCategory::Lantern ? gpu_lanterns_
        : p.category == PropCategory::House    ? gpu_houses_
        : p.category == PropCategory::Wall     ? gpu_walls_
        : p.category == PropCategory::Gate     ? gpu_gates_
        : p.category == PropCategory::Well     ? gpu_wells_
        : p.category == PropCategory::Bridge   ? gpu_bridges_
        : p.category == PropCategory::Market   ? gpu_markets_
        : p.category == PropCategory::Path     ? gpu_paths_
        : p.category == PropCategory::Planter  ? gpu_planters_
        : p.category == PropCategory::Fountain  ? gpu_fountains_
        : p.category == PropCategory::Decor     ? gpu_decor_
        : p.category == PropCategory::River     ? gpu_rivers_
        : p.category == PropCategory::Crystal   ? gpu_crystals_
        : p.category == PropCategory::GlowShroom ? gpu_glow_shrooms_
        : p.category == PropCategory::Campfire  ? gpu_campfires_
        : p.category == PropCategory::Monument  ? gpu_monuments_
        : p.category == PropCategory::Watchtower ? gpu_watchtowers_
                                               : gpu_fountains_;
    if (set.empty()) {
        return;
    }
    const GpuProp& gp = set[p.variant % set.size()];
    const Mat4 m = glm::translate(Mat4{1.0f}, p.position) *
                   glm::rotate(Mat4{1.0f}, p.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                   glm::scale(Mat4{1.0f}, Vec3{p.scale * p.length, p.scale, p.scale});
    const f32 glow = glm::mix(1.0f, 0.45f, sun_intensity_); // emissive dims by day

    // Fade a house roof when the local player is inside its footprint.
    bool inside = false;
    if (gp.footprint.x > 0.0f) {
        // Into the house's local frame: the mesh is drawn rotateY(yaw), so undo it with rotateY(-yaw)
        // (which in xz is a standard 2D rotation by +yaw).
        const Vec3 rel = local_feet() - p.position;
        const f32 cs = std::cos(p.yaw);
        const f32 sn = std::sin(p.yaw);
        const Vec2 lp{rel.x * cs - rel.z * sn, rel.x * sn + rel.z * cs};
        inside = std::abs(lp.x) < gp.footprint.x + 0.4f && std::abs(lp.y) < gp.footprint.y + 0.4f &&
                 rel.y > -1.5f && rel.y < gp.wall_height + 2.0f;
    }
    // A burning cottage chars toward black and its cosy window-glow gives way to
    // flames (drawn separately in draw_fires).
    const f32 burn = gp.footprint.x > 0.0f ? house_burn(p.position) : 0.0f;
    const f32 cozy = 1.0f - burn;          // cosy emissive / interior light fades out
    const Vec3 charcoal = glm::mix(Vec3{1.0f}, Vec3{0.14f, 0.12f, 0.11f}, burn);
    const Vec4 char_tint{charcoal, 1.0f};

    const f32 night = 1.0f - sun_intensity_;
    // Firelight breathes: a per-prop low-frequency flicker (phase seeded from the prop's
    // position so every lantern/window beats to its own rhythm) applied to both the
    // emissive glow and the spot lights below - candlelight instead of a dead-flat glow.
    const f32 flick_phase =
        glm::fract(std::sin(p.position.x * 12.9898f + p.position.z * 78.233f) * 43758.5453f) *
        TwoPi;
    const f32 flicker = 1.0f + 0.05f * std::sin(elapsed_ * 7.3f + flick_phase) +
                        0.03f * std::sin(elapsed_ * 11.9f + flick_phase * 1.7f);

    for (const GpuPropPart& part : gp.parts) {
        if (part.layer == PropLayer::Foliage) {
            renderer_->draw_transparent(part.mesh, m, Vec4{1.0f});
        } else if (part.layer == PropLayer::Emissive) {
            // Only flicker where the glow is actually firelight (night); by day the
            // emissive is nearly off and a wobble would read as shimmer.
            const f32 e = glow * cozy * glm::mix(1.0f, flicker, night);
            renderer_->draw_emissive(part.mesh, m, Vec4{e, e, e, 1.0f});
        } else if (part.layer == PropLayer::Glow) {
            // Window light shafts: additive, dusk/night only, gone once ablaze.
            if (night > 0.05f && cozy > 0.05f) {
                renderer_->draw_glow(part.mesh, m, Vec4{1.0f, 1.0f, 1.0f, night * 0.6f * cozy});
            }
        } else if (part.layer == PropLayer::Roof) {
            if (inside) {
                renderer_->draw_transparent(part.mesh, m, Vec4{charcoal, 0.18f});
            } else {
                renderer_->draw(part.mesh, m, char_tint);
            }
        } else {
            renderer_->draw(part.mesh, m, char_tint);
        }
    }

    // Interior + lantern spot lights, added at dusk/night. Only props near the player
    // submit lights at all (distant town lights are skipped) - this keeps the lit-up
    // night cheap. House interiors cast shadows (occluded by walls); lanterns/braziers
    // are cheap unshadowed pools, so a town full of lanterns barely costs anything.
    const bool near_player = glm::length(p.position - local_feet()) < light::prop_cull_dist;
    if (night > 0.12f && cozy > 0.1f && near_player && !gp.lights.empty()) {
        const Mat3 rot{m};
        for (const PropLight& pl : gp.lights) {
            Renderer::SpotLight sl;
            sl.position = Vec3{m * Vec4{pl.offset, 1.0f}};
            sl.direction = glm::normalize(rot * pl.direction);
            // The flicker also warms slightly as it dims (ember) and cools as it
            // brightens (flame lick), like real firelight.
            const Vec3 warm_shift = glm::mix(Vec3{1.06f, 0.97f, 0.88f}, Vec3{0.97f, 1.0f, 1.08f},
                                             glm::clamp((flicker - 0.94f) * 6.0f, 0.0f, 1.0f));
            sl.color = pl.color * warm_shift * (pl.intensity * night * cozy * flicker);
            sl.range = pl.range;
            const f32 half = glm::radians(pl.cone_deg * 0.5f);
            sl.cone_outer_cos = std::cos(half);
            sl.cone_inner_cos = std::cos(half * 0.65f);
            // House hearth/lamp/candle light is INDOORS: it must be occluded by the walls,
            // so it casts a real shadow (or isn't drawn). A standalone WILD lantern (one
            // dotted through the forest, not inside a town) also casts shadows so it grounds
            // the player + props in the dark woods. Town lanterns / gate braziers stay in
            // the cheap unshadowed pool (a whole town of them would swamp the shadow budget).
            const bool wild_lantern =
                p.category == PropCategory::Lantern &&
                !worldgen::inside_village(p.position.x, p.position.z, world_seed_, 2.0f);
            sl.indoor = (p.category == PropCategory::House);
            sl.cast_shadow = (p.category == PropCategory::House) || wild_lantern;
            renderer_->add_light(sl);
        }
    }

    // Chimney smoke: soft grey puffs rising from the chimney pot on a deterministic loop
    // (pure function of time - nothing to simulate, spawn or cull), swaying as they climb
    // and leaning with the storm wind. Intact houses near the player only.
    if (gp.chimney_spot.y > 0.01f && cozy > 0.5f && near_player) {
        const Vec3 stack = Vec3{m * Vec4{gp.chimney_spot, 1.0f}};
        const f32 lean = 0.3f + weather_amt_ * 1.6f; // wind pushes the column over
        for (int i = 0; i < 4; ++i) {
            const f32 ph = glm::fract(elapsed_ * 0.10f + flick_phase * 0.159f +
                                      static_cast<f32>(i) * 0.25f); // 0 at the pot, 1 dispersed
            const f32 rise = ph * 3.4f;
            const f32 sway = std::sin(elapsed_ * 0.8f + flick_phase + static_cast<f32>(i) * 2.1f) *
                             (0.08f + 0.3f * ph);
            const Vec3 at = stack + Vec3{sway + lean * rise * 0.35f, rise, sway * 0.7f};
            const f32 size = 0.16f + ph * 0.6f;                     // puffs grow as they thin
            const f32 alpha =
                0.32f * (1.0f - ph) * glm::smoothstep(0.0f, 0.1f, ph); // fade in at the pot, out on top
            renderer_->draw_transparent(shape_sphere_,
                                        glm::translate(Mat4{1.0f}, at) *
                                            glm::scale(Mat4{1.0f}, Vec3{size}),
                                        Vec4{0.60f, 0.59f, 0.58f, alpha});
        }
    }
}

void ClientApp::update_villager_visuals(Timestep dt) {
    if (!have_snapshot_) {
        return;
    }
    for (const net::VillagerState& vl : snapshot_.villagers) {
        PlayerVisual& v = ensure_villager_visual(vl.id, vl.appearance, vl.kind);
        f32 measured = 0.0f;
        if (v.has_last && dt.seconds > 0.0001f) {
            Vec3 d = vl.position - v.last_pos;
            d.y = 0.0f;
            measured = glm::length(d) / dt.seconds;
        }
        v.speed = glm::mix(v.speed, measured, 0.3f);
        v.last_pos = vl.position;
        v.has_last = true;
        v.animator.update(v.speed, dt);
    }
    for (auto it = villager_visuals_.begin(); it != villager_visuals_.end();) {
        const bool live = std::any_of(snapshot_.villagers.begin(), snapshot_.villagers.end(),
                                      [&](const net::VillagerState& s) { return s.id == it->first; });
        if (live) {
            ++it;
        } else {
            retire_mesh(std::move(it->second.body_mesh)); // defer the GPU free past the frames in flight
            retire_mesh(std::move(it->second.outfit_mesh));
            it = villager_visuals_.erase(it);
        }
    }
}

ClientApp::PlayerVisual& ClientApp::ensure_villager_visual(u32 id,
                                                           const CharacterAppearance& appearance,
                                                           u8 kind) {
    const auto it = villager_visuals_.find(id);
    if (it != villager_visuals_.end()) {
        return it->second;
    }
    PlayerVisual v;
    v.appearance = appearance;
    v.model = CharacterModel::create(id ^ 0x55u, appearance);
    Equipment eq;
    if (kind == 4) {
        // A NOBLE passenger: the reference-grade (Master) plate - gilded armour with a plume - which
        // the client gold-tints in draw_villagers, plus a large flowing red cape (setup_noble_cape).
        eq.outfit_tier = static_cast<u8>(EquipmentTier::Master);
        apply_outfit(v.model, OutfitKind::Plate, eq);
        v.body_skin = build_body_mesh(v.model);
        v.outfit_skin = build_outfit_mesh(v.model, OutfitKind::Plate, eq);
        setup_noble_cape(v);
    } else {
        // Generic peasant garb (a belted tunic + hose + headwear), varied per NPC: the tint's bits pick
        // the tunic, the headwear (hood / coif / straw hat / bare) and the hood's dye.
        eq.outfit_tint = static_cast<u8>((id * 2654435761u) >> 26);
        apply_outfit(v.model, OutfitKind::Peasant, eq);
        v.body_skin = build_body_mesh(v.model);
        v.outfit_skin = build_outfit_mesh(v.model, OutfitKind::Peasant, eq);
    }
    return villager_visuals_.emplace(id, std::move(v)).first->second;
}

void ClientApp::draw_villagers() {
    if (!have_snapshot_) {
        return;
    }
    for (const net::VillagerState& vl : snapshot_.villagers) {
        const auto it = villager_visuals_.find(vl.id);
        if (it == villager_visuals_.end()) {
            continue;
        }
        PlayerVisual& v = it->second;
        // Kind 3 is a hired carriage driver (top seat) and kind 4 a noble passenger riding inside -
        // both sit (sit pose) attached to the cart's tilt + bob so they ride with it.
        const bool seated = vl.kind == 3 || vl.kind == 4;
        const net::WagonState* aw = seated ? active_wagon() : nullptr;
        const Vec3 seat = (aw != nullptr) ? attach_to_wagon(*aw, vl.position) : vl.position;
        const Vec3 base = seated ? seat - Vec3{0.0f, seat_drop(v.model), 0.0f} : vl.position;
        Mat4 root = glm::translate(Mat4{1.0f}, base) *
                    glm::rotate(Mat4{1.0f}, HalfPi - vl.yaw, Vec3{0.0f, 1.0f, 0.0f});
        if (!seated) {
            root = root * v.animator.body_offset();
        }
        const std::vector<Quat> pose =
            seated ? CharacterAnimator::sit_pose(v.model) : v.animator.pose(v.model);
        // Guards (kind 1) wear a steel tint + carry a spear; wall archers (kind 2) wear a
        // leather/steel tint + hold a bow; the rest are plain townsfolk.
        const Vec3 tint = vl.kind == 1   ? Vec3{0.72f, 0.76f, 0.86f}
                          : vl.kind == 2 ? Vec3{0.66f, 0.70f, 0.80f}
                          : vl.kind == 4 ? Vec3{1.0f, 0.84f, 0.42f} // noble: gilded armour
                                         : Vec3{1.0f};
        if (v.body_skin.vertices.empty()) {
            v.body_skin = build_body_mesh(v.model);
        }
        skin_and_draw(v.model, v.body_skin, v.body_mesh, root, pose, tint);     // continuous skinned body
        skin_and_draw(v.model, v.outfit_skin, v.outfit_mesh, root, pose, tint); // peasant tunic + trousers
        const std::vector<Mat4> mats = v.model.bone_matrices(root, pose);
        draw_rig(v.model, mats, tint, /*attachments_only=*/true); // face/hair/cap/apron on top
        if (vl.kind == 4) {
            // The noble's grand crimson cape (simulated cloth) at the joint frames; tint {1} keeps
            // it red rather than gilding it like the plate.
            draw_cloth(v, root, v.model.joint_matrices(root, pose), Vec3{1.0f}, base.y);
        }
        if (vl.kind == 1) {
            draw_held_spear(v.model, mats);
        } else if (vl.kind == 2) {
            const std::vector<Bone>& bones = v.model.bones();
            int hand = -1;
            for (usize i = 0; i < bones.size(); ++i) {
                if (bones[i].part == BonePart::LowerArmR) {
                    hand = static_cast<int>(i);
                    break;
                }
            }
            if (hand >= 0) {
                const Vec3 grip = Vec3{mats[hand][3]};
                const Vec3 fwd{std::cos(vl.yaw), 0.0f, std::sin(vl.yaw)};
                const Vec3 hpos = grip + fwd * 0.12f;
                const Vec4 wood{0.4f, 0.26f, 0.12f, 1.0f};
                renderer_->draw(shape_box_,
                                glm::translate(Mat4{1.0f}, hpos) *
                                    glm::scale(Mat4{1.0f}, Vec3{0.05f, 1.2f, 0.06f}),
                                wood); // bow stave
                for (f32 s : {1.0f, -1.0f}) {
                    renderer_->draw(shape_box_,
                                    glm::translate(Mat4{1.0f}, hpos + Vec3{0.05f, s * 0.55f, 0.0f}) *
                                        glm::scale(Mat4{1.0f}, Vec3{0.05f, 0.2f, 0.05f}),
                                    wood); // recurved tips
                }
            }
        }
    }
}

Mat4 ClientApp::orient_to(const Vec3& dir) {
    const f32 len = glm::length(dir);
    if (len < 1e-4f) {
        return Mat4{1.0f};
    }
    const Vec3 f = dir / len;
    const Vec3 up = std::abs(f.y) > 0.99f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 r = glm::normalize(glm::cross(up, f));
    const Vec3 u = glm::cross(f, r);
    Mat4 m{1.0f};
    m[0] = Vec4{r, 0.0f};
    m[1] = Vec4{u, 0.0f};
    m[2] = Vec4{f, 0.0f};
    return m;
}

void ClientApp::draw_walls() {
    if (!have_snapshot_ || renderer_ == nullptr) {
        return;
    }
    for (const net::WallState& wl : snapshot_.walls) {
        const f32 hp = static_cast<f32>(wl.health) / 255.0f;
        const f32 len = wl.length;
        const int n = std::max(4, static_cast<int>(len));
        // Same rotated frame as the server's box collider (translate * rotateY(yaw)), so the
        // visible chunks line up with what NPCs path around.
        const Mat4 root = glm::translate(Mat4{1.0f}, wl.position) *
                          glm::rotate(Mat4{1.0f}, wl.yaw, Vec3{0.0f, 1.0f, 0.0f});
        for (int i = 0; i < n; ++i) {
            const u32 h = static_cast<u32>(i) * 2654435761u +
                          static_cast<u32>(std::lround(wl.position.x * 7.0f) * 40503);
            const f32 r0 = static_cast<f32>(h & 255u) / 255.0f;
            const f32 r1 = static_cast<f32>((h >> 8) & 255u) / 255.0f;
            const f32 r2 = static_cast<f32>((h >> 16) & 255u) / 255.0f;
            const f32 lx = ((static_cast<f32>(i) + 0.5f) / static_cast<f32>(n) - 0.5f) * len;
            const f32 hh = kRockWallHeight * (0.7f + 0.5f * r0) * (0.4f + 0.6f * hp); // crumbles
            const f32 cw = len / static_cast<f32>(n) * (0.75f + 0.4f * r1);
            const Vec3 col = glm::mix(Vec3{0.24f, 0.22f, 0.20f}, Vec3{0.48f, 0.45f, 0.42f}, r2) *
                             (0.5f + 0.5f * hp);
            const Mat4 m = root *
                           glm::translate(Mat4{1.0f}, Vec3{lx, hh * 0.5f, (r1 - 0.5f) * 0.3f}) *
                           glm::rotate(Mat4{1.0f}, (r2 - 0.5f) * 0.4f, Vec3{0.0f, 1.0f, 0.0f}) *
                           glm::scale(Mat4{1.0f}, Vec3{cw, hh, kRockWallThick * (0.8f + 0.3f * r0)});
            renderer_->draw(shape_box_, m, Vec4{col, 1.0f});
        }
    }
}

} // namespace alryn::game
