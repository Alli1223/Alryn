// Roles, weapons & abilities (see Game/Roles.h). Server-authoritative: each tick the
// server adopts every player's chosen role (applying its stats + walk speed), ticks the
// per-ability cooldowns + buff timers, and - when a player invokes an ability this tick -
// resolves it against the live ambush enemies / fellow players / projectile list. Kept in
// its own translation unit alongside Contracts.cpp / SiegeMode.cpp; the combat state
// (players_ / ambush_ / projectiles_) lives on GameServer.

#include <Alryn/Core/Log.h>
#include <Alryn/Game/Roles.h>
#include <Alryn/Net/GameServer.h>

#include <cmath>

namespace alryn {

// Adopt the role the client is reporting: switch stats (and heal to full on a change),
// keep health clamped to the role's max, and drive the controller's walk speed (with the
// Hunter's dash boost folded in).
void GameServer::sync_player_role(ServerPlayer& player) {
    const auto requested = static_cast<PlayerRole>(player.input.role % kRoleCount);
    if (requested != player.role) {
        player.role = requested;
        player.max_health = role_stats(requested).max_health;
        player.health = player.max_health; // a fresh kit starts whole
    }
    player.max_health = role_stats(player.role).max_health;
    player.health = std::min(player.health, player.max_health);
    const f32 base = role_stats(player.role).move_speed;
    player.controller.set_walk_speed(player.dash_timer > 0.0f ? base * kDashSpeedMult : base);
}

void GameServer::update_abilities(Timestep dt, const DensitySampler& density) {
    const f32 dts = dt.seconds;
    for (auto& [id, pl] : players_) {
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

        const u8 slot_one = pl.input.ability; // 0 = none, else slot+1
        if (slot_one == 0 || slot_one > kAbilitySlots) {
            continue;
        }
        const u8 slot = static_cast<u8>(slot_one - 1);
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
                } else { // Taunt: nearby enemies fixate on this Knight
                    for (Enemy& e : ambush_) {
                        if (glm::length(e.position - pl.controller.position()) <= kTauntRadius) {
                            e.taunt_by = id;
                            e.taunt_cd = kTauntDuration;
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
                } else { // Dash: a burst of speed
                    pl.dash_timer = kDashDuration;
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
                } else { // Smite: a holy bolt toward the aim point
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
                }
                break;
        }

        pl.ability_cd[slot] = ability_def(pl.role, slot).cooldown;
        pl.cast_fx = slot_one; // echoed in the snapshot so every client plays the cast VFX
    }

    (void)density;
}

} // namespace alryn
