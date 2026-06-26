// Roles, weapons & abilities (see Game/Roles.h). Server-authoritative: each tick the
// server adopts every player's chosen role (applying its stats + walk speed), ticks the
// per-ability cooldowns + buff timers, and - when a player invokes an ability this tick -
// resolves it against the live ambush enemies / fellow players / projectile list. Kept in
// its own translation unit alongside Contracts.cpp / SiegeMode.cpp; the combat state
// (players_ / ambush_ / projectiles_) lives on GameServer.

#include <Alryn/Core/Log.h>
#include <Alryn/Game/Roles.h>
#include <Alryn/Net/GameServer.h>

#include <algorithm>
#include <cmath>

namespace alryn {

// Adopt the role the client is reporting: switch stats (and heal to full on a change),
// keep health clamped to the role's max, and drive the controller's walk speed (with the
// Hunter's dash boost folded in).
void GameServer::sync_player_role(ServerPlayer& player) {
    // Accept the player's cosmetic loadout choices; clamp the EARNED tiers to what they've bought
    // (owned_tier), so a client can't claim master gear it didn't pay for.
    player.equipment.outfit_tint = player.input.equipment.outfit_tint;
    player.equipment.weapon_index = player.input.equipment.weapon_index;
    player.equipment.outfit_tier = std::min<u8>(player.input.equipment.outfit_tier, player.owned_tier);
    player.equipment.weapon_tier = std::min<u8>(player.input.equipment.weapon_tier, player.owned_tier);
    const f32 hp_bonus = equipment_bonus(player.equipment).health_add; // armour adds max health

    const auto requested = static_cast<PlayerRole>(player.input.role % kRoleCount);
    if (requested != player.role) {
        player.role = requested;
        player.health = role_stats(requested).max_health + hp_bonus; // a fresh kit starts whole
    }
    player.max_health = role_stats(player.role).max_health + hp_bonus;
    player.health = std::min(player.health, player.max_health);
    f32 speed = role_stats(player.role).move_speed;
    if (player.dash_timer > 0.0f) {
        speed *= kDashSpeedMult; // Hunter Dash
    }
    if (player.haste_timer > 0.0f) {
        speed *= kHasteMult; // War Horn (co-op haste)
    }
    player.controller.set_walk_speed(speed);
}

void GameServer::update_abilities(Timestep dt, const DensitySampler& density) {
    const f32 dts = dt.seconds;
    const u32 seed = sampler_.seed();
    for (auto& [id, pl] : players_) {
        // Town shop: buy gear tiers up to the requested target, one at a time, while standing in a
        // town and the party can afford the next tier (server-authoritative purchase -> owned_tier).
        const Vec3 pp = pl.controller.position();
        while (pl.input.buy > pl.owned_tier && static_cast<u8>(pl.owned_tier + 1) < kTierCount &&
               worldgen::inside_village(pp.x, pp.z, seed, 6.0f)) {
            const u32 price = tier_price(static_cast<EquipmentTier>(pl.owned_tier + 1));
            if (money_ < price) {
                break;
            }
            money_ -= price;
            pl.owned_tier = static_cast<u8>(pl.owned_tier + 1);
        }
        // Town shop: buy WAGON-RIG upgrade levels up to the requested target (a money sink), one at a
        // time, while in a town + the party can afford the next level. buy_rig is the target level.
        while (pl.input.buy_rig > rig_level_ && rig_level_ < kMaxRigLevel &&
               worldgen::inside_village(pp.x, pp.z, seed, 6.0f)) {
            const u32 price = rig_price(static_cast<u8>(rig_level_ + 1));
            if (money_ < price) {
                break;
            }
            money_ -= price;
            rig_level_ = static_cast<u8>(rig_level_ + 1);
        }
        sync_player_role(pl);
        pl.cast_fx = 0; // cleared each tick; set below when an ability actually fires
        for (f32& cd : pl.ability_cd) {
            if (cd > 0.0f) {
                cd -= dts;
            }
        }
        if (pl.bulwark_timer > 0.0f) {
            pl.bulwark_timer -= dts;
        }
        if (pl.dash_timer > 0.0f) {
            pl.dash_timer -= dts;
        }
        if (pl.damage_boost_timer > 0.0f) { // Empower (co-op damage buff)
            pl.damage_boost_timer -= dts;
        }
        if (pl.haste_timer > 0.0f) { // War Horn (co-op speed buff)
            pl.haste_timer -= dts;
        }
        if (pl.shield_timer > 0.0f) { // Aegis fades with time, or breaks when fully spent
            pl.shield_timer -= dts;
            if (pl.shield_timer <= 0.0f || pl.shield_hp <= 0.0f) {
                pl.shield_hp = 0.0f;
                pl.shield_timer = 0.0f;
            }
        }

        const u8 ability_one = pl.input.ability; // 0 = none, else ability index + 1
        if (ability_one == 0 || ability_one > kAbilityCount) {
            continue;
        }
        const u8 slot = static_cast<u8>(ability_one - 1); // ability index (into ability_def)
        if (pl.ability_cd[slot] > 0.0f) {
            continue; // still cooling down
        }

        const Vec3 origin = pl.controller.position() + Vec3{0.0f, 0.9f, 0.0f};
        const Vec3 eye = pl.controller.eye_position();
        const f32 yaw = pl.input.yaw;
        const Vec3 facing{std::cos(yaw), 0.0f, std::sin(yaw)};
        const RoleStats stats = role_stats(pl.role);

        switch (pl.role) {
            case PlayerRole::Knight:
                if (slot == 0) { // Shield Bash: hit every enemy in a frontal cone, knock them back
                    for (Enemy& e : ambush_) {
                        const Vec3 chest = e.position + Vec3{0.0f, 0.9f, 0.0f};
                        if (!in_attack_cone(origin, yaw, chest, kMeleeRange + 0.5f, kMeleeConeCos)) {
                            continue;
                        }
                        e.health -= kBashDamage;
                        Vec3 push = e.position - pl.controller.position();
                        push.y = 0.0f;
                        if (glm::length(push) > 1e-3f) {
                            push = glm::normalize(push);
                        } else {
                            push = facing;
                        }
                        e.position += push * kBashKnockback;
                    }
                } else if (slot == 1) { // Bulwark: raise the shield for heavy mitigation
                    pl.bulwark_timer = kBulwarkDuration;
                } else if (slot == 2) { // Consecration: a holy ground aura that taunts + burns
                    spawn_aura(AuraKind::Consecration, pl.controller.position(), id);
                } else if (slot == 3) { // Taunt: nearby enemies instantly fixate on this Knight
                    for (Enemy& e : ambush_) {
                        if (glm::length(e.position - pl.controller.position()) <= kTauntRadius) {
                            e.taunt_by = id;
                            e.taunt_cd = kTauntDuration;
                        }
                    }
                } else if (slot == 4) { // Whirlwind: a 360 cleave that hits everything around you
                    for (Enemy& e : ambush_) {
                        Vec3 d = e.position - pl.controller.position();
                        d.y = 0.0f;
                        if (glm::length(d) > kWhirlwindRadius) {
                            continue;
                        }
                        e.health -= kWhirlwindDamage;
                        const Vec3 push = glm::length(d) > 1e-3f ? glm::normalize(d) : facing;
                        e.position += push * kWhirlwindKnockback;
                    }
                } else if (slot == 5) { // Rally: heal self + nearby allies + brief bulwark
                    for (auto& [oid, other] : players_) {
                        if (glm::length(other.controller.position() - pl.controller.position()) <=
                            kHealRadius) {
                            other.health = std::min(other.max_health, other.health + kRallyHeal);
                        }
                    }
                    pl.bulwark_timer = kBulwarkDuration;
                } else { // Guardian Leap: jump to the nearest ally, shield them, pull their attackers
                    ServerPlayer* ally = nullptr;
                    f32 best = kGuardLeapRange;
                    for (auto& [oid, other] : players_) {
                        if (oid == id) {
                            continue;
                        }
                        const f32 d =
                            glm::length(other.controller.position() - pl.controller.position());
                        if (d < best) {
                            best = d;
                            ally = &other;
                        }
                    }
                    if (ally != nullptr) {
                        const Vec3 ap = ally->controller.position();
                        Vec3 to = pl.controller.position() - ap;
                        to.y = 0.0f;
                        to = glm::length(to) > 1e-3f ? glm::normalize(to) : facing;
                        pl.controller.set_position(ap + to * 1.6f); // land just beside the ally
                        ally->shield_hp = kGuardShieldAmount; // protect them
                        ally->shield_timer = kAegisDuration;
                        for (Enemy& e : ambush_) { // pull their attackers onto the Knight
                            if (glm::length(e.position - ap) <= kGuardTauntRadius) {
                                e.taunt_by = id;
                                e.taunt_cd = kTauntDuration;
                            }
                        }
                    }
                }
                break;

            case PlayerRole::Hunter: {
                auto loose_arrow = [&](const Vec3& dir, f32 damage) {
                    Projectile pr;
                    pr.position = eye + dir * 0.6f;
                    pr.velocity = dir * 36.0f;
                    pr.owner = id;
                    pr.damage = damage;
                    pr.kind = 3; // friendly arrow (slim bolt + trail on the client)
                    pr.radius = 0.15f;
                    pr.life = 3.5f;
                    projectiles_.push_back(pr);
                };
                Vec3 dir = pl.input.aim - eye;
                dir = glm::length(dir) > 0.2f ? glm::normalize(dir) : facing;
                if (slot == 0) { // Power Shot: one heavy arrow
                    loose_arrow(dir, stats.ranged_damage * kPowerShotMult);
                } else if (slot == 1) { // Volley: a three-arrow spread
                    for (int k = -1; k <= 1; ++k) {
                        const f32 a = std::atan2(dir.z, dir.x) + static_cast<f32>(k) * 0.18f;
                        loose_arrow(Vec3{std::cos(a), dir.y, std::sin(a)}, stats.ranged_damage * kVolleyMult);
                    }
                } else if (slot == 2) { // Dash: a burst of speed
                    pl.dash_timer = kDashDuration;
                } else if (slot == 3) { // Piercing Shot: a single heavy bolt
                    loose_arrow(dir, stats.ranged_damage * kPierceMult);
                } else if (slot == 4) { // Multishot: a wide five-arrow fan
                    const f32 base = std::atan2(dir.z, dir.x);
                    const int half = kMultishotArrows / 2;
                    for (int k = -half; k <= half; ++k) {
                        const f32 a = base + static_cast<f32>(k) * 0.16f;
                        loose_arrow(Vec3{std::cos(a), dir.y, std::sin(a)},
                                    stats.ranged_damage * kMultishotMult);
                    }
                } else if (slot == 5) { // Caltrops: a hazard ground aura at the aim point
                    spawn_aura(AuraKind::Hazard, pl.input.aim, id);
                } else { // War Horn: haste self + nearby allies (co-op)
                    for (auto& [oid, other] : players_) {
                        if (glm::length(other.controller.position() - pl.controller.position()) <=
                            kHasteRadius) {
                            other.haste_timer = kHasteDuration;
                        }
                    }
                }
                break;
            }

            case PlayerRole::Cleric:
                if (slot == 0) { // Heal: mend the most-injured ally in range (self counts)
                    ServerPlayer* target = &pl;
                    f32 worst = pl.health / pl.max_health;
                    for (auto& [oid, other] : players_) {
                        if (glm::length(other.controller.position() - pl.controller.position()) > kHealRadius) {
                            continue;
                        }
                        const f32 frac = other.health / other.max_health;
                        if (frac < worst) {
                            worst = frac;
                            target = &other;
                        }
                    }
                    target->health = std::min(target->max_health, target->health + kHealAmount);
                } else if (slot == 1) { // Sanctuary: heal everyone nearby
                    for (auto& [oid, other] : players_) {
                        if (glm::length(other.controller.position() - pl.controller.position()) <= kHealRadius) {
                            other.health = std::min(other.max_health, other.health + kSanctuaryAmount);
                        }
                    }
                } else if (slot == 2) { // Smite: a holy bolt toward the aim point
                    Vec3 dir = pl.input.aim - eye;
                    dir = glm::length(dir) > 0.2f ? glm::normalize(dir) : facing;
                    Projectile pr;
                    pr.position = eye + dir * 0.6f;
                    pr.velocity = dir * 28.0f;
                    pr.owner = id;
                    pr.damage = kSmiteDamage;
                    pr.kind = 2; // holy bolt (rendered bright by the client)
                    pr.radius = 0.2f;
                    pr.life = 3.0f;
                    projectiles_.push_back(pr);
                } else if (slot == 3) { // Aegis: shield the nearest friendly player/NPC (closest)
                    ServerPlayer* tp = nullptr;
                    f32 pbest = kAegisRange;
                    for (auto& [oid, other] : players_) {
                        const f32 d = glm::length(other.controller.position() - pl.controller.position());
                        if (d < pbest) {
                            pbest = d;
                            tp = &other;
                        }
                    }
                    Villager* tv = nullptr;
                    f32 vbest = kAegisRange;
                    for (auto& [vid, vg] : villagers_) {
                        const f32 d = glm::length(vg.position - pl.controller.position());
                        if (d < vbest) {
                            vbest = d;
                            tv = &vg;
                        }
                    }
                    if (tv != nullptr && vbest < pbest) {
                        tv->shield_timer = kAegisDuration; // NPCs take no damage here -> visual ward
                    } else if (tp != nullptr) {
                        tp->shield_hp = kAegisAmount; // players get a real damage-absorb pool
                        tp->shield_timer = kAegisDuration;
                    }
                } else if (slot == 4) { // Renew: lay a lingering heal aura at the caster's feet
                    spawn_aura(AuraKind::Heal, pl.controller.position(), id);
                } else if (slot == 5) { // Judgement: a heavy holy bolt toward the aim point
                    Vec3 dir = pl.input.aim - eye;
                    dir = glm::length(dir) > 0.2f ? glm::normalize(dir) : facing;
                    Projectile pr;
                    pr.position = eye + dir * 0.6f;
                    pr.velocity = dir * 30.0f;
                    pr.owner = id;
                    pr.damage = kJudgementDamage;
                    pr.kind = 2; // holy bolt (rendered bright by the client)
                    pr.radius = 0.22f;
                    pr.life = 3.0f;
                    projectiles_.push_back(pr);
                } else { // Empower: bless the nearest ally so their attacks hit harder (co-op)
                    ServerPlayer* ally = nullptr;
                    f32 best = kEmpowerRange;
                    for (auto& [oid, other] : players_) {
                        if (oid == id) {
                            continue;
                        }
                        const f32 d =
                            glm::length(other.controller.position() - pl.controller.position());
                        if (d < best) {
                            best = d;
                            ally = &other;
                        }
                    }
                    ServerPlayer* target = ally != nullptr ? ally : &pl; // solo -> empower self
                    target->damage_boost_timer = kDamageBoostDuration;
                }
                break;

            case PlayerRole::Mage:
                break; // the Mage casts via combos (input.spell -> update_spells), not the hotbar
        }

        pl.ability_cd[slot] = ability_def(pl.role, slot).cooldown;
        pl.cast_fx = ability_one; // echoed in the snapshot so every client plays the cast VFX
    }

    update_auras(dt);
    (void)density;
}

// Ground auras. A Cleric holds right mouse (input.block) to CHANNEL a heavy heal: the charge
// builds while held and, when full, drops a heal aura at their feet (then resets). A Knight's
// Consecration drops a holy aura that taunts + burns enemies. Each tick every live aura applies
// its effect to whoever stands inside, then expires when its lifetime runs out.
// Creates a ground aura of `kind`, taking its radius + lifetime from the shared aura_props table.
void GameServer::spawn_aura(AuraKind kind, const Vec3& pos, net::PlayerId owner) {
    const AuraProps p = aura_props(kind);
    auras_.push_back({pos, p.duration, p.radius, static_cast<u8>(kind), owner});
}

void GameServer::update_auras(Timestep dt) {
    const f32 dts = dt.seconds;
    for (auto& [id, pl] : players_) {
        if (pl.role == PlayerRole::Cleric && pl.input.block) {
            pl.heal_charge += dts;
            if (pl.heal_charge >= kHealChargeTime) {
                spawn_aura(AuraKind::Heal, pl.controller.position(), id);
                pl.heal_charge = 0.0f;
            }
        } else {
            pl.heal_charge = 0.0f; // releasing early (or any non-Cleric) fizzles the channel
        }
    }
    for (auto& [vid, vg] : villagers_) { // Aegis shields on NPCs fade with time (visual only)
        if (vg.shield_timer > 0.0f) {
            vg.shield_timer -= dts;
        }
    }
    for (Aura& a : auras_) {
        a.ttl -= dts;
        switch (static_cast<AuraKind>(a.kind)) { // per-kind effect (add a case for a new aura type)
            case AuraKind::Heal: // mend allies standing in it
                for (auto& [id, pl] : players_) {
                    if (pl.health > 0.0f &&
                        glm::length(pl.controller.position() - a.position) <= a.radius) {
                        pl.health = std::min(pl.max_health, pl.health + kHealAuraRate * dts);
                    }
                }
                break;
            case AuraKind::Consecration: // taunt + burn enemies standing in it
                for (Enemy& e : ambush_) {
                    if (e.alive && glm::length(e.position - a.position) <= a.radius) {
                        e.taunt_by = a.owner;
                        e.taunt_cd = kTauntDuration;
                        e.health -= kConsecrationDPS * dts;
                    }
                }
                break;
            case AuraKind::Hazard: // wound enemies standing in it (Hunter caltrops, no taunt)
                for (Enemy& e : ambush_) {
                    if (e.alive && glm::length(e.position - a.position) <= a.radius) {
                        e.health -= kHazardDPS * dts;
                    }
                }
                break;
        }
    }
    std::erase_if(auras_, [](const Aura& a) { return a.ttl <= 0.0f; });
}

// --- Mage elemental combo spells ------------------------------------------------------------
// The client resolves the queued element combo into a SpellId and sends it in PlayerInput.spell;
// here the server gates it on the Mage's single spell cooldown and applies the authoritative
// effect. Elemental bolts are friendly Projectiles (new kinds the client renders); Meteor is an
// instant AoE; Healing Bloom mends nearby allies; Rock Wall raises a collider NPCs route around.
void GameServer::update_spells(Timestep dt, const DensitySampler& density) {
    for (auto& [id, pl] : players_) {
        if (pl.spell_cd > 0.0f) {
            pl.spell_cd -= dt.seconds;
        }
        if (pl.role != PlayerRole::Mage) {
            continue;
        }
        const auto sp = static_cast<SpellId>(pl.input.spell);
        if (sp == SpellId::None || pl.spell_cd > 0.0f) {
            continue;
        }
        cast_spell(pl, id, sp);
        pl.spell_cd = spell_cooldown(sp);
        pl.cast_fx = pl.input.spell; // echoed in the snapshot so clients play the spell VFX
    }
    update_walls(dt);
    (void)density;
}

void GameServer::cast_spell(ServerPlayer& pl, net::PlayerId id, SpellId spell) {
    const Vec3 eye = pl.controller.eye_position();
    const f32 yaw = pl.input.yaw;
    const Vec3 facing{std::cos(yaw), 0.0f, std::sin(yaw)};
    Vec3 dir = pl.input.aim - eye;
    dir = glm::length(dir) > 0.2f ? glm::normalize(dir) : facing;

    auto bolt = [&](f32 speed, f32 dmg, u8 kind, f32 radius, f32 life) {
        Projectile pr;
        pr.position = eye + dir * 0.6f;
        pr.velocity = dir * speed;
        pr.owner = id;
        pr.damage = dmg;
        pr.kind = kind; // 5 = fireball, 6 = frost bolt, 7 = boulder (client-rendered)
        pr.radius = radius;
        pr.life = life;
        projectiles_.push_back(pr);
    };

    switch (spell) {
        case SpellId::Fireball: bolt(30.0f, kFireballDamage, 5, 0.3f, 3.0f); break;
        case SpellId::FrostBolt: bolt(34.0f, kFrostBoltDamage, 6, 0.25f, 3.0f); break;
        case SpellId::Boulder: bolt(22.0f, kBoulderDamage, 7, 0.4f, 3.5f); break;
        case SpellId::Meteor: // an instant blast at the aim point
            for (Enemy& e : ambush_) {
                if (e.alive && glm::length(e.position - pl.input.aim) <= kMeteorRadius) {
                    e.health -= kMeteorDamage * pl.outgoing_mult();
                }
            }
            break;
        case SpellId::HealBloom: // mend allies around the caster
            for (auto& [oid, other] : players_) {
                if (glm::length(other.controller.position() - pl.controller.position()) <=
                    kHealBloomRadius) {
                    other.health = std::min(other.max_health, other.health + kHealBloomAmount);
                }
            }
            break;
        case SpellId::Empower: // bless nearby allies' attacks (co-op buff)
            for (auto& [oid, other] : players_) {
                if (glm::length(other.controller.position() - pl.controller.position()) <=
                    kHealBloomRadius) {
                    other.damage_boost_timer = kDamageBoostDuration;
                }
            }
            break;
        case SpellId::RockWall: { // raise a wall of stone in front, spanning across the facing
            Wall w;
            w.position = pl.controller.position() + facing * kRockWallAhead;
            w.yaw = yaw + 1.5707963f; // the wall SPANS perpendicular to where the Mage faces
            w.length = kRockWallLength;
            w.health = kRockWallHealth;
            w.ttl = kRockWallTtl;
            walls_.push_back(w);
            break;
        }
        default: break;
    }
}

void GameServer::update_walls(Timestep dt) {
    for (Wall& w : walls_) {
        w.ttl -= dt.seconds;
    }
    std::erase_if(walls_, [](const Wall& w) { return w.ttl <= 0.0f || w.health <= 0.0f; });
}

// A single box collider spanning the wall (long axis = its span, thin across, tall enough to
// block a capsule). `yaw` is negated to match the client's translate * rotateY(yaw) * scale.
void GameServer::wall_colliders(const Wall& w, std::vector<Collider>& out) {
    Collider c;
    c.shape = Collider::Shape::Box;
    c.center = w.position;
    c.half = Vec2{w.length * 0.5f, kRockWallThick * 0.5f};
    c.yaw = -w.yaw;
    c.y_min = w.position.y;
    c.y_max = w.position.y + kRockWallHeight;
    out.push_back(c);
}

void GameServer::append_walls(const Vec3& pos, std::vector<Collider>& out) const {
    for (const Wall& w : walls_) {
        if (glm::length(Vec2{w.position.x - pos.x, w.position.z - pos.z}) < 14.0f) {
            wall_colliders(w, out);
        }
    }
}

} // namespace alryn
