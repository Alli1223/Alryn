// Preserved night-siege simulation (tower-defence Phases 1-9). These were once driven
// by GameServer each tick; the game has since pivoted to goods transport, so they are
// NO LONGER CALLED. The full implementation is kept here, verbatim, so the nightly
// assault (waves, torch-bearers, guards, house fires, barricades, the day/night match
// loop) can be re-enabled later by re-wiring the calls back into GameServer::tick.
// See CLAUDE.md sections 6.14-6.23 for the design.
#include <Alryn/Net/GameServer.h>

#include <Alryn/Core/Density.h>
#include <Alryn/Core/Log.h>
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/World/Village.h>

#include <cmath>
#include <cstdlib>

namespace alryn {

namespace {
// Terrain-carve fallback for the melee swing (when there's nothing to hit).
constexpr f32 kEditRadius = 2.5f;
constexpr f32 kEditAmount = 2.0f;

// Match rhythm: the enemy attacks ONLY at night. Day is the prep phase (build,
// repair, regroup); at dusk a wave spawns and reinforcements keep coming until dawn,
// when the survivors flee the daylight.
constexpr f32 kVerdictHold = 8.0f;     // how long the win/lose banner shows before restart
constexpr f32 kRepairRate = 0.5f;      // ruin mended per second of repairing
constexpr f32 kNightWaveInterval = 13.0f; // seconds between reinforcement waves at night
constexpr u32 kWaveSize = 6;
constexpr usize kMaxEnemies = 48;
constexpr f32 kSpawnRing = 24.0f;  // how far out enemies appear
constexpr f32 kAggroRadius = 15.0f; // an enemy chases a target within this range
constexpr u32 kNightsToWin = 3;     // survive this many nights with houses standing

// House-burning (Phase 4): only a TORCH-bearing enemy (kind 1) can torch a house;
// fire then grows on its own until the house is lost.
constexpr f32 kHouseBurnRange = 4.5f;  // to the house centre - just past its walls
constexpr f32 kFireHitAmount = 0.16f;  // fire added per torch swing at a house
constexpr f32 kFireGrowRate = 0.045f;  // fire/sec once alight (it spreads)
constexpr f32 kEmberIntensity = 0.28f; // the glow a burnt-down ruin keeps

// Town guards: NPCs that hunt + fight the enemy.
constexpr f32 kGuardMaxHealth = 80.0f;
constexpr f32 kGuardSpeed = 2.9f;
constexpr f32 kGuardAggro = 22.0f;       // a guard charges an enemy within this range
constexpr f32 kGuardAttackRange = 1.7f;
constexpr f32 kGuardDamage = 16.0f;
constexpr f32 kGuardAttackInterval = 0.9f;
constexpr u32 kGuardsPerTown = 4;

// Player-built barricades: block + slow the enemy; they hack through them.
constexpr f32 kBarricadeMaxHealth = 120.0f;
constexpr f32 kBarricadeDamage = 14.0f;     // enemy hit per swing
constexpr f32 kBarricadeRange = 1.6f;
constexpr usize kMaxBarricades = 24;
constexpr i32 kBuildStock = 8; // barricades a player can raise per day

// Archers (enemy kind 3): kite to range and loose hostile arrows.
constexpr f32 kArcherShootRange = 16.0f;
constexpr f32 kArcherKeepDist = 9.0f; // backs off if a target gets nearer than this
constexpr f32 kArcherInterval = 2.3f; // seconds between shots
constexpr f32 kArrowSpeed = 20.0f;
constexpr f32 kArrowDamage = 9.0f;

// A small deterministic hash -> [0,1), so wave layouts are reproducible.
f32 hash01(u32 a, u32 b) {
    u32 v = a * 2654435761u ^ (b + 0x9E3779B9u + (a << 6) + (a >> 2));
    v ^= v >> 15;
    v *= 0x2545F491u;
    v ^= v >> 13;
    return static_cast<f32>(v & 0xFFFFFFu) / static_cast<f32>(0x1000000u);
}
} // namespace

// A player's melee swing: strike the nearest enemy inside the reach cone in front
// of them. If no enemy is hit, the swing falls back to carving terrain at `aim`, so
// the left mouse button still digs when there's nothing to fight.
void GameServer::player_attack(ServerPlayer& player, const net::PlayerInput& in) {
    const Vec3 origin = player.controller.position() + Vec3{0.0f, 0.9f, 0.0f};
    Enemy* best = nullptr;
    f32 best_d = kMeleeRange + 1.0f;
    for (Enemy& en : enemies_) {
        if (!en.alive) {
            continue;
        }
        const Vec3 chest = en.position + Vec3{0.0f, 0.9f, 0.0f};
        if (!in_attack_cone(origin, in.yaw, chest, kMeleeRange, kMeleeConeCos)) {
            continue;
        }
        const f32 d = glm::length(chest - origin);
        if (d < best_d) {
            best_d = d;
            best = &en;
        }
    }
    if (best != nullptr) {
        best->health -= kMeleeDamage;
        return;
    }
    // Nothing to hit: dig where aimed (same as the old left-click carve).
    sampler_.add_edit(in.aim, kEditRadius, kEditAmount);
    server_.broadcast_deform(net::DeformEvent{in.aim, kEditRadius, kEditAmount});
}

// Places a defensive barricade just in front of the player - but only by DAY (the
// prep phase), so you fortify between the night assaults. Won't stack on an existing
// one, and the town can only hold so many.
void GameServer::player_build(ServerPlayer& player, const net::PlayerInput& in) {
    if (phase_ != net::MatchPhase::Prep || player.wood <= 0 ||
        barricades_.size() >= kMaxBarricades) {
        return;
    }
    const Vec3 pos = player.controller.position();
    Vec3 spot = pos + Vec3{std::cos(in.yaw), 0.0f, std::sin(in.yaw)} * 1.9f;
    const DensitySampler density = sampler_.as_sampler();
    if (const auto g = raycast_density(density, Vec3{spot.x, spot.y + 6.0f, spot.z},
                                       Vec3{0.0f, -1.0f, 0.0f}, 14.0f)) {
        spot.y = g->y;
    }
    for (const Barricade& b : barricades_) {
        if (glm::length(b.position - spot) < 1.5f) {
            return; // don't stack
        }
    }
    Barricade b;
    b.position = spot;
    b.yaw = in.yaw + HalfPi; // the wall spans across the player's facing
    b.health = kBarricadeMaxHealth;
    barricades_.push_back(b);
    --player.wood;
    ALRYN_INFO("Barricade raised ({} up, {} timber left)", barricades_.size(), player.wood);
}

// A box collider for a barricade (a low wall ~2m wide). Matches the client's
// translate * rotateY(yaw) * scale, like place_box, so collision lines up with the mesh.
Collider GameServer::barricade_collider(const Barricade& b) {
    Collider c;
    c.shape = Collider::Shape::Box;
    c.center = b.position;
    c.half = Vec2{1.0f, 0.28f};
    c.yaw = -b.yaw;
    c.y_min = b.position.y;
    c.y_max = b.position.y + 1.3f;
    return c;
}

// Appends the colliders of barricades near `pos` to `out` (so enemies/players are
// blocked by them on top of the world colliders).
void GameServer::append_barricades(const Vec3& pos, std::vector<Collider>& out) const {
    for (const Barricade& b : barricades_) {
        if (glm::length(Vec2{b.position.x - pos.x, b.position.z - pos.z}) < 6.0f) {
            out.push_back(barricade_collider(b));
        }
    }
}

// Spawns a batch of enemies and points them at the area the players defend. If
// they're in a town, the wave enters **through the gates** (the gaps in the
// palisade) and marches on the plaza; otherwise it spawns in a ring around the
// players' centroid. Enemies chase any villager/player that strays close.
void GameServer::spawn_wave() {
    const u32 seed = sampler_.seed();

    // Centre the assault on the nearest town, or the players' centroid.
    Vec3 centroid{0.0f};
    for (const auto& [id, player] : players_) {
        centroid += player.controller.position();
    }
    centroid /= static_cast<f32>(players_.size());

    Vec3 target = centroid;
    std::optional<worldgen::Village> town;
    const int vcx = static_cast<int>(std::floor(centroid.x / worldgen::village_cell));
    const int vcz = static_cast<int>(std::floor(centroid.z / worldgen::village_cell));
    f32 best = 1e30f;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (const auto v = worldgen::village_at(vcx + dx, vcz + dz, seed)) {
                const Vec3 c{v->center.x, centroid.y, v->center.y};
                const f32 d = glm::length(Vec2{c.x - centroid.x, c.z - centroid.z});
                if (d < best) {
                    best = d;
                    target = c;
                    town = v;
                }
            }
        }
    }

    // Entry points: the town's gates (a little outside each gap), else a ring.
    std::vector<Vec3> entries;
    if (town) {
        for (const Vec3& g : village_gates(*town, seed)) {
            Vec3 outward = g - target;
            outward.y = 0.0f;
            if (glm::length(outward) > 1e-3f) {
                outward = glm::normalize(outward);
            }
            entries.push_back(g + outward * 2.0f); // just outside the gateway
        }
    }

    const u32 si = spawn_index_++; // distinct layout per spawn (nights have several)
    const DensitySampler density = sampler_.as_sampler();
    for (u32 i = 0; i < kWaveSize && enemies_.size() < kMaxEnemies; ++i) {
        Enemy en;
        en.id = next_enemy_id_++;
        // A mix of kinds: ~1 in 6 a brute (2), ~1 in 6 an archer (3), the rest split
        // grunts (0) / torch-bearers (1).
        const u32 roll = (si * 7u + i) % 6u;
        en.kind = roll == 2u ? 2u : roll == 5u ? 3u : static_cast<u8>((si + i) % 2u);
        en.health = enemy_max_health(en.kind);
        en.home = target;
        if (!entries.empty()) {
            const Vec3& gate = entries[(si + i) % entries.size()];
            const f32 j = (hash01(si, i * 7u + 5u) - 0.5f) * 3.0f; // spread along the gap
            en.position = Vec3{gate.x + j, gate.y + 30.0f, gate.z + j};
        } else {
            const f32 ang = hash01(si, i * 7u + 1u) * TwoPi;
            const f32 r = kSpawnRing + hash01(si, i * 7u + 3u) * 6.0f;
            en.position = Vec3{target.x + std::cos(ang) * r, target.y + 30.0f,
                               target.z + std::sin(ang) * r};
        }
        if (const auto g = raycast_density(density, en.position, Vec3{0.0f, -1.0f, 0.0f}, 80.0f)) {
            en.position.y = g->y;
        }
        enemies_.push_back(en);
    }
    ALRYN_INFO("A wave of {} attacks ({} total, night {})", kWaveSize, enemies_.size(), wave_);
}

void GameServer::update_enemies(Timestep dt, const DensitySampler& density) {
    for (Enemy& en : enemies_) {
        if (collision_) {
            collision_->gather(en.position, collider_scratch_);
        }
        append_barricades(en.position, collider_scratch_); // player defences block them
        // Two breeds: **torch-bearers** (kind 1) make straight for the buildings to set
        // them alight; the rest hunt the nearest villager/player/guard. Only torch
        // enemies can burn houses - so the player must intercept them.
        const bool arsonist = en.kind == 1;
        Vec3 goal = en.home;
        ServerPlayer* pvictim = nullptr;
        Villager* vvictim = nullptr;
        f32 best = en.kind == 3 ? kArcherShootRange + 3.0f : kAggroRadius; // archers see further
        if (!arsonist) {
            for (auto& [id, player] : players_) {
                const f32 d = glm::length(player.controller.position() - en.position);
                if (d < best) {
                    best = d;
                    pvictim = &player;
                    vvictim = nullptr;
                    goal = player.controller.position();
                }
            }
            for (auto& [id, vl] : villagers_) {
                if (!vl.alive) {
                    continue;
                }
                const f32 d = glm::length(vl.position - en.position);
                if (d < best) {
                    best = d;
                    vvictim = &vl;
                    pvictim = nullptr;
                    goal = vl.position;
                }
            }
        }

        // March on the nearest standing house (arsonists always; others only when no
        // villager/player is near) and set it alight.
        HouseFire* hvictim = nullptr;
        f32 house_d = 1e30f;
        if (pvictim == nullptr && vvictim == nullptr) {
            for (auto& [id, h] : houses_) {
                if (h.destroyed) {
                    continue;
                }
                const f32 d = glm::length(h.position - en.position);
                if (d < house_d) {
                    house_d = d;
                    hvictim = &h;
                }
            }
            if (hvictim != nullptr) {
                goal = hvictim->position;
            }
        }
        // Archers (kind 3) kite to range and loose hostile arrows instead of meleeing.
        if (en.kind == 3) {
            const Vec3 target = goal; // the chosen living target (or home if none)
            const bool has_target = (pvictim != nullptr || vvictim != nullptr);
            Vec3 g = goal;
            if (has_target && best < kArcherShootRange) {
                g = en.position; // hold and shoot
                if (best < kArcherKeepDist) {
                    Vec3 away = en.position - target;
                    away.y = 0.0f;
                    if (glm::length(away) > 1e-3f) {
                        g = en.position + glm::normalize(away) * 5.0f; // back off
                    }
                }
            }
            step_enemy(en, density, collider_scratch_, g, dt, kEnemySpeed);
            if (en.attack_cd > 0.0f) {
                en.attack_cd -= dt.seconds;
            }
            if (has_target && best < kArcherShootRange && en.attack_cd <= 0.0f) {
                const Vec3 from = en.position + Vec3{0.0f, 1.3f, 0.0f};
                Vec3 dir = (target + Vec3{0.0f, 0.9f, 0.0f}) - from;
                if (glm::length(dir) > 0.3f) {
                    dir = glm::normalize(dir);
                    Projectile arrow;
                    arrow.position = from + dir * 0.5f;
                    arrow.velocity = dir * kArrowSpeed;
                    arrow.kind = 1;
                    arrow.hostile = true;
                    arrow.radius = 0.15f;
                    arrow.life = 3.0f;
                    projectiles_.push_back(arrow);
                    en.attack_cd = kArcherInterval;
                }
            }
            continue; // archers don't melee / burn houses
        }

        const f32 spd = en.kind == 2 ? kEnemySpeed * 0.62f : kEnemySpeed; // brutes lumber
        const f32 dmg = en.kind == 2 ? kEnemyAttackDamage * 2.3f : kEnemyAttackDamage;
        step_enemy(en, density, collider_scratch_, goal, dt, spd);

        // On cooldown: strike a living target within reach; else hack a barricade
        // we're pressed against; else (torch-bearers only) set fire to a nearby house.
        if (en.attack_cd <= 0.0f) {
            const f32 reach = kEnemyAttackRange + kEnemyRadius;
            if (pvictim != nullptr && best < reach) {
                pvictim->health -= dmg;
                pvictim->since_hit = 0.0f;
                en.attack_cd = kEnemyAttackInterval;
            } else if (vvictim != nullptr && best < reach) {
                vvictim->health -= dmg; // villager or guard
                en.attack_cd = kEnemyAttackInterval;
            } else {
                Barricade* block = nullptr;
                f32 bd = kBarricadeRange;
                for (Barricade& b : barricades_) {
                    const f32 d = glm::length(b.position - en.position);
                    if (d < bd) {
                        bd = d;
                        block = &b;
                    }
                }
                if (block != nullptr) {
                    block->health -= kBarricadeDamage; // chop through the defence
                    en.attack_cd = kEnemyAttackInterval;
                } else if (en.kind == 1 && pvictim == nullptr && vvictim == nullptr) {
                    // A torch-bearer with no living target burns the nearest house.
                    HouseFire* near = nullptr;
                    f32 nd = kHouseBurnRange;
                    for (auto& [id, h] : houses_) {
                        if (h.destroyed) {
                            continue;
                        }
                        const f32 d = glm::length(h.position - en.position);
                        if (d < nd) {
                            nd = d;
                            near = &h;
                        }
                    }
                    if (near != nullptr) {
                        near->fire = std::min(1.0f, near->fire + kFireHitAmount);
                        en.attack_cd = kEnemyAttackInterval;
                    }
                }
            }
        }
    }
    std::erase_if(barricades_, [](const Barricade& b) { return b.health <= 0.0f; });
}

// Townsfolk: spawn one per cottage in towns near the players (deterministic per
// town+house), then steer them - flee from a close enemy, sleep in their bed at
// night, otherwise wander the plaza. Villagers far from every player are culled
// (they respawn identically when a player returns); the dead stay down.
void GameServer::update_villagers(Timestep dt, const DensitySampler& density) {
    const u32 seed = sampler_.seed();
    const bool day = time_of_day_ > 0.27f && time_of_day_ < 0.78f;

    // Ensure a villager exists for every cottage in towns near a player.
    for (const auto& [pid, player] : players_) {
        const Vec3 focus = player.controller.position();
        const int vcx = static_cast<int>(std::floor(focus.x / worldgen::village_cell));
        const int vcz = static_cast<int>(std::floor(focus.z / worldgen::village_cell));
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto v = worldgen::village_at(vcx + dx, vcz + dz, seed);
                if (!v) {
                    continue;
                }
                u32 hi = 0;
                for (const PropInstance& pr : village_props(*v, seed)) {
                    if (pr.category != PropCategory::House) {
                        continue;
                    }
                    const u32 id = (v->vseed * 2654435761u) ^ ((hi + 1u) * 40499u);
                    const u32 house_id = (v->vseed * 2654435761u) ^ ((hi + 1u) * 49957u);
                    ++hi;
                    // Register the burnable house once (keeps its fire across re-visits
                    // unless culled), and remember the town's house total for the tally.
                    if (houses_.count(house_id) == 0u) {
                        HouseFire hf;
                        hf.position = pr.position;
                        hf.yaw = pr.yaw;
                        hf.vseed = v->vseed;
                        houses_.emplace(house_id, hf);
                        town_house_total_[v->vseed] = std::max(town_house_total_[v->vseed], hi);
                    }
                    if (villagers_.count(id) != 0u) {
                        continue;
                    }
                    const f32 cs = std::cos(pr.yaw);
                    const f32 sn = std::sin(pr.yaw);
                    auto to_world = [&](const Vec3& l) {
                        return pr.position + Vec3{l.x * cs + l.z * sn, l.y, -l.x * sn + l.z * cs};
                    };
                    const PropDef& hd = prop_lib_.resolve(pr); // this house variant's layout
                    Villager vg;
                    vg.id = id;
                    vg.appearance = villager_look(id);
                    vg.bed = to_world(hd.bed_spot);
                    vg.position = to_world(hd.door_spot);
                    vg.position.y = worldgen::height(vg.position.x, vg.position.z, seed);
                    vg.target = vg.position;
                    vg.home_center = v->center;
                    vg.home_half = v->half;
                    vg.rng = id | 1u;
                    villagers_.emplace(id, std::move(vg));
                }
            }
        }
    }

    // Town guards: a handful per town who patrol the plaza and hunt the enemy.
    for (const auto& [pid, player] : players_) {
        const Vec3 focus = player.controller.position();
        const int vcx = static_cast<int>(std::floor(focus.x / worldgen::village_cell));
        const int vcz = static_cast<int>(std::floor(focus.z / worldgen::village_cell));
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto v = worldgen::village_at(vcx + dx, vcz + dz, seed);
                if (!v) {
                    continue;
                }
                for (u32 gi = 0; gi < kGuardsPerTown; ++gi) {
                    const u32 id = (v->vseed * 2654435761u) ^ ((gi + 1u) * 2246822519u);
                    if (villagers_.count(id) != 0u) {
                        continue;
                    }
                    const f32 a = TwoPi * static_cast<f32>(gi) / static_cast<f32>(kGuardsPerTown);
                    Villager g;
                    g.id = id;
                    g.kind = 1;
                    g.health = kGuardMaxHealth;
                    g.appearance = villager_look(id ^ 0xABCDu);
                    g.position = Vec3{v->center.x + std::cos(a) * 6.0f, 0.0f,
                                      v->center.y + std::sin(a) * 6.0f};
                    g.position.y = worldgen::height(g.position.x, g.position.z, seed);
                    g.target = g.position;
                    g.home_center = v->center;
                    g.home_half = v->half;
                    g.rng = id | 1u;
                    villagers_.emplace(id, std::move(g));
                }
            }
        }
    }

    auto nearest_player = [&](const Vec3& p) {
        f32 best = 1e30f;
        for (const auto& [pid, player] : players_) {
            best = std::min(best, glm::length(player.controller.position() - p));
        }
        return best;
    };
    auto rnd = [](u32& s) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<f32>(s & 0xFFFFFFu) / static_cast<f32>(0xFFFFFFu);
    };

    for (auto it = villagers_.begin(); it != villagers_.end();) {
        Villager& vg = it->second;
        if (vg.health <= 0.0f) {
            vg.alive = false;
        }
        if (!vg.alive || nearest_player(vg.position) > 115.0f) {
            it = villagers_.erase(it); // dead, or out of range (respawns deterministically)
            continue;
        }
        if (collision_) {
            collision_->gather(vg.position, collider_scratch_);
        }
        append_barricades(vg.position, collider_scratch_);

        // Guards: charge the nearest enemy and cut it down; otherwise patrol the plaza.
        if (vg.kind == 1) {
            if (vg.attack_cd > 0.0f) {
                vg.attack_cd -= dt.seconds;
            }
            Enemy* prey = nullptr;
            f32 gbest = kGuardAggro;
            for (Enemy& en : enemies_) {
                if (!en.alive) {
                    continue;
                }
                const f32 d = glm::length(en.position - vg.position);
                if (d < gbest) {
                    gbest = d;
                    prey = &en;
                }
            }
            if (prey != nullptr) {
                vg.target = prey->position;
                if (gbest < kGuardAttackRange + kEnemyRadius && vg.attack_cd <= 0.0f) {
                    prey->health -= kGuardDamage;
                    vg.attack_cd = kGuardAttackInterval;
                }
                step_villager(vg, density, collider_scratch_, vg.target, dt, kGuardSpeed);
            } else {
                Vec3 to = vg.target - vg.position;
                to.y = 0.0f;
                if (glm::length(to) < 0.5f) {
                    vg.wait -= dt.seconds;
                    if (vg.wait <= 0.0f) {
                        const f32 a = rnd(vg.rng) * TwoPi;
                        const f32 rr = 3.0f + rnd(vg.rng) * (vg.home_half * 0.55f);
                        vg.target = Vec3{vg.home_center.x + std::cos(a) * rr, vg.position.y,
                                         vg.home_center.y + std::sin(a) * rr};
                        vg.wait = 1.0f + rnd(vg.rng) * 2.0f;
                    }
                }
                step_villager(vg, density, collider_scratch_, vg.target, dt, kVillagerSpeed);
            }
            ++it;
            continue;
        }

        // Flee a nearby enemy; else sleep at night / wander by day.
        f32 threat = kVillagerFleeRadius;
        const Enemy* danger = nullptr;
        for (const Enemy& en : enemies_) {
            const f32 d = glm::length(en.position - vg.position);
            if (en.alive && d < threat) {
                threat = d;
                danger = &en;
            }
        }
        // During the prep lull, villagers also rebuild burnt-down ruins; mid-wave they
        // only douse active fires.
        const bool prep = phase_ == net::MatchPhase::Prep;
        HouseFire* blaze = nearest_fire(vg.position, 55.0f, prep);
        const Vec3 well{vg.home_center.x, vg.position.y, vg.home_center.y};
        f32 speed = kVillagerSpeed;
        if (danger != nullptr) {
            // Run for the safety of the plaza (and the player defending it) rather
            // than blindly away, so the town huddles together under attack.
            vg.target = well;
            speed = kVillagerFleeSpeed;
        } else if (blaze != nullptr) {
            // Bucket brigade: fetch water/materials from the well, then douse the fire
            // (or, in the lull, mend the ruin back into a standing house).
            speed = kVillagerSpeed * 1.35f; // hustle to the job
            if (vg.water <= 0.0f) {
                vg.target = well;
                if (glm::length(Vec2{well.x - vg.position.x, well.z - vg.position.z}) < kWellRange) {
                    vg.water = 1.0f; // fill the bucket
                }
            } else {
                vg.target = blaze->position;
                if (glm::length(blaze->position - vg.position) < kDouseRange) {
                    const f32 rate = prep ? kRepairRate : kDouseRate;
                    blaze->fire = std::max(0.0f, blaze->fire - rate * dt.seconds);
                    vg.water = std::max(0.0f, vg.water - kWaterUseRate * dt.seconds);
                    if (prep && blaze->fire <= 0.0f) {
                        blaze->destroyed = false; // rebuilt
                    }
                }
            }
        } else if (!day) {
            vg.target = vg.bed;
        } else {
            Vec3 to = vg.target - vg.position;
            to.y = 0.0f;
            if (glm::length(to) < 0.4f) {
                vg.wait -= dt.seconds;
                if (vg.wait <= 0.0f) {
                    const f32 a = rnd(vg.rng) * TwoPi;
                    const f32 rr = rnd(vg.rng) * (vg.home_half - 4.0f);
                    vg.target = Vec3{vg.home_center.x + std::cos(a) * rr, vg.position.y,
                                     vg.home_center.y + std::sin(a) * rr};
                    vg.wait = 1.0f + rnd(vg.rng) * 3.0f;
                }
            }
        }
        step_villager(vg, density, collider_scratch_, vg.target, dt, speed);
        ++it;
    }
}

GameServer::HouseFire* GameServer::nearest_fire(const Vec3& p, f32 max_dist, bool include_ruins) {
    HouseFire* best = nullptr;
    f32 bd = max_dist;
    for (auto& [id, h] : houses_) {
        const bool damaged = include_ruins ? (h.fire > 0.0f || h.destroyed) : !h.destroyed && h.fire > 0.0f;
        if (!damaged) {
            continue;
        }
        const f32 d = glm::length(h.position - p);
        if (d < bd) {
            bd = d;
            best = &h;
        }
    }
    return best;
}

std::optional<Vec3> GameServer::nearest_well(const Vec3& p) const {
    const u32 seed = sampler_.seed();
    const int vcx = static_cast<int>(std::floor(p.x / worldgen::village_cell));
    const int vcz = static_cast<int>(std::floor(p.z / worldgen::village_cell));
    std::optional<Vec3> best;
    f32 bd = 1e30f;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (const auto v = worldgen::village_at(vcx + dx, vcz + dz, seed)) {
                const f32 d = glm::length(Vec2{v->center.x - p.x, v->center.y - p.z});
                if (d < bd) {
                    bd = d;
                    best = Vec3{v->center.x, p.y, v->center.y}; // the well sits at the plaza centre
                }
            }
        }
    }
    return best;
}

// Players are bucket-carriers too: stand by the well to fill, stand by a fire to pour
// (or, in the prep lull, by a ruin to rebuild it).
void GameServer::update_player_firefighting(Timestep dt) {
    const bool prep = phase_ == net::MatchPhase::Prep;
    for (auto& [id, player] : players_) {
        const Vec3 pos = player.controller.position();
        if (const auto well = nearest_well(pos)) {
            if (glm::length(Vec2{well->x - pos.x, well->z - pos.z}) < kWellRange) {
                player.water = 1.0f;
            }
        }
        if (player.water > 0.0f) {
            if (HouseFire* fire = nearest_fire(pos, kDouseRange, prep)) {
                const f32 rate = prep ? kRepairRate : kDouseRate;
                fire->fire = std::max(0.0f, fire->fire - rate * dt.seconds);
                player.water = std::max(0.0f, player.water - kWaterUseRate * dt.seconds);
                if (prep && fire->fire <= 0.0f) {
                    fire->destroyed = false; // rebuilt
                }
            }
        }
    }
}

// Fire spread + the win/lose verdict. Ignited houses burn on their own until they
// collapse into smouldering embers; the match is judged for the town nearest the
// players - lose if it is razed, win if the players outlast the waves with it standing.
void GameServer::update_fires(Timestep dt) {
    for (auto& [id, h] : houses_) {
        if (!h.destroyed && h.fire > 0.0f) {
            h.fire += kFireGrowRate * dt.seconds;
            if (h.fire >= 1.0f) {
                h.fire = kEmberIntensity; // collapses to a glowing ruin
                h.destroyed = true;
                ALRYN_INFO("A house has burnt down");
            }
        }
    }

    // Cull houses far from every player (they re-register when a player returns).
    if (!players_.empty()) {
        for (auto it = houses_.begin(); it != houses_.end();) {
            f32 best = 1e30f;
            for (const auto& [pid, player] : players_) {
                best = std::min(best, glm::length(player.controller.position() - it->second.position));
            }
            if (best > 140.0f) {
                it = houses_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Judge the match for the town nearest the players.
    houses_standing_ = 0;
    houses_total_ = 0;
    if (players_.empty()) {
        return;
    }
    Vec3 centroid{0.0f};
    for (const auto& [pid, player] : players_) {
        centroid += player.controller.position();
    }
    centroid /= static_cast<f32>(players_.size());

    const u32 seed = sampler_.seed();
    const int vcx = static_cast<int>(std::floor(centroid.x / worldgen::village_cell));
    const int vcz = static_cast<int>(std::floor(centroid.z / worldgen::village_cell));
    std::optional<u32> town_seed;
    f32 best = 1e30f;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (const auto v = worldgen::village_at(vcx + dx, vcz + dz, seed)) {
                const f32 d = glm::length(Vec2{v->center.x - centroid.x, v->center.y - centroid.z});
                if (d < best) {
                    best = d;
                    town_seed = v->vseed;
                }
            }
        }
    }
    if (!town_seed) {
        return; // not defending a town right now
    }

    const u32 total = town_house_total_.count(*town_seed) ? town_house_total_[*town_seed] : 0u;
    u32 standing = 0;
    for (const auto& [id, h] : houses_) {
        if (h.vseed == *town_seed && !h.destroyed) {
            ++standing;
        }
    }
    houses_total_ = static_cast<u8>(std::min<u32>(total, 255u));
    houses_standing_ = static_cast<u8>(std::min<u32>(standing, 255u));

    // Defeat: the town is razed. (Victory is decided by the phase machine once the
    // players outlast the waves.) Hold the banner, then restart.
    if (outcome_ == net::MatchOutcome::Ongoing && total > 0 && standing == 0) {
        outcome_ = net::MatchOutcome::Lost;
        reset_timer_ = kVerdictHold;
        ALRYN_INFO("The village was razed - defeat");
    }
}

// The prep<->combat rhythm: a calm lull (repair + brace) then a wave that runs until
// it's cleared. Surviving kWavesToWin waves wins; a verdict holds its banner, then
// the match restarts fresh so play continues.
void GameServer::update_phases(Timestep dt) {
    if (players_.empty()) {
        return;
    }
    // The enemy attacks only at NIGHT; daytime is the prep phase. phase_timer_ is the
    // HUD countdown to the next dusk/dawn.
    const bool night = time_of_day_ <= 0.27f || time_of_day_ >= 0.78f;
    const f32 frac = !night ? (0.78f - time_of_day_)
                            : ((time_of_day_ >= 0.78f ? 1.27f : 0.27f) - time_of_day_);
    phase_timer_ = frac * day_seconds_;

    if (outcome_ != net::MatchOutcome::Ongoing) {
        reset_timer_ -= dt.seconds;
        if (reset_timer_ <= 0.0f) {
            reset_match();
        }
        was_night_ = night;
        return;
    }

    if (night && !was_night_) {
        // Dusk: the assault begins.
        phase_ = net::MatchPhase::Combat;
        ++wave_; // = the night number
        spawn_wave();
        night_wave_timer_ = kNightWaveInterval;
        ALRYN_INFO("Night {} falls - the enemy comes", wave_);
    } else if (!night && was_night_) {
        // Dawn: the survivors flee the daylight; the town is resupplied with timber.
        phase_ = net::MatchPhase::Prep;
        enemies_.clear();
        for (auto& [id, player] : players_) {
            player.wood = kBuildStock;
        }
        if (wave_ >= kNightsToWin) {
            outcome_ = net::MatchOutcome::Won;
            reset_timer_ = kVerdictHold;
            ALRYN_INFO("The village endured {} nights - victory!", wave_);
        } else {
            ALRYN_INFO("Dawn breaks - night {} survived; prepare for the next", wave_);
        }
    }

    // Reinforcements keep coming through the night.
    if (night && phase_ == net::MatchPhase::Combat) {
        night_wave_timer_ -= dt.seconds;
        if (night_wave_timer_ <= 0.0f && enemies_.size() < kMaxEnemies) {
            spawn_wave();
            night_wave_timer_ = kNightWaveInterval;
        }
    }
    was_night_ = night;
}

// Wipe the round back to its opening state for a fresh siege (after a verdict).
void GameServer::reset_match() {
    enemies_.clear();
    projectiles_.clear();
    villagers_.clear(); // respawn at full health
    houses_.clear();    // re-register intact next tick
    barricades_.clear();
    town_house_total_.clear();
    wave_ = 0;
    const bool night = time_of_day_ <= 0.27f || time_of_day_ >= 0.78f;
    phase_ = night ? net::MatchPhase::Combat : net::MatchPhase::Prep;
    was_night_ = night;
    night_wave_timer_ = 1.0f;
    reset_timer_ = 0.0f;
    outcome_ = net::MatchOutcome::Ongoing;
    for (auto& [id, player] : players_) {
        player.health = kPlayerMaxHealth;
        player.water = 0.0f;
    }
    ALRYN_INFO("A new watch begins - defend the village");
}

} // namespace alryn
