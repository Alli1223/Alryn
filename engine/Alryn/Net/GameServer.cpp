#include <Alryn/Net/GameServer.h>

#include <Alryn/Core/Density.h>
#include <Alryn/Core/Log.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/World/VehicleTypes.h>
#include <Alryn/World/Village.h>

#include <cmath>
#include <cstdlib>

namespace alryn {

namespace {
constexpr f32 kEditRadius = 2.5f;
constexpr f32 kEditAmount = 2.0f;
constexpr f32 kProjectileSpeed = 24.0f;
constexpr usize kMaxProjectiles = 256;
} // namespace

bool GameServer::start(u16 port, u32 seed, u32 max_clients) {
    sampler_.set_seed(seed);
    collision_.emplace(seed, prop_lib_);
    manager_.init(); // owns the server-authoritative day/night clock (ALRYN_TIME / _DAY_SECONDS)
    if (!server_.start(port, max_clients)) {
        return false;
    }
    ALRYN_INFO("Game server up (seed {})", seed);
    return true;
}

void GameServer::stop() {
    server_.stop();
    players_.clear();
}

Vec3 GameServer::spawn_point(net::PlayerId id) const {
    const u32 seed = sampler_.seed();
    const f32 base_x = -4.0f + static_cast<f32>(id % 5u) * 2.0f;
    const f32 base_z = -4.0f + static_cast<f32>((id / 5u) % 5u) * 2.0f;

    f32 x = base_x;
    f32 z = base_z;

    // Spawn in the nearest village so you land right in town. Scan the village grid
    // outward from the origin in rings; the first ring with a town wins, and within
    // it the town closest to the origin. A small per-player offset spreads players
    // around the central plaza.
    const int ocx = static_cast<int>(std::floor(base_x / worldgen::village_cell));
    const int ocz = static_cast<int>(std::floor(base_z / worldgen::village_cell));
    bool village_found = false;
    f32 best = 1e30f;
    for (int r = 0; r <= 6 && !village_found; ++r) {
        for (int dz = -r; dz <= r; ++dz) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) {
                    continue; // only the shell of ring r
                }
                if (const auto v = worldgen::village_at(ocx + dx, ocz + dz, seed)) {
                    // Only spawn in a town that's actually road-connected to others, so the player
                    // always starts somewhere they can take a haul from (never a stranded town).
                    if (roads::reachable_towns(v->center, seed, 1).empty()) {
                        continue;
                    }
                    const f32 d = glm::length(v->center);
                    if (d < best) {
                        best = d;
                        x = v->center.x + base_x * 0.4f; // land in the plaza, spread out
                        z = v->center.y + base_z * 0.4f;
                        village_found = true;
                    }
                }
            }
        }
    }

    // No town nearby: spiral outward to a forested dry spot, then any dry land.
    if (!village_found) {
        bool land_found = false;
        bool forest_found = false;
        for (int i = 0; i < 200 && !forest_found; ++i) {
            const f32 cx = base_x + std::cos(static_cast<f32>(i) * 2.4f) * static_cast<f32>(i) * 1.2f;
            const f32 cz = base_z + std::sin(static_cast<f32>(i) * 2.4f) * static_cast<f32>(i) * 1.2f;
            const f32 h = worldgen::height(cx, cz, seed);
            if (h <= worldgen::water_level + 0.8f) {
                continue;
            }
            if (!land_found) {
                x = cx;
                z = cz;
                land_found = true;
            }
            if (h < 7.0f && worldgen::moisture(cx, cz, seed) > 0.15f) {
                x = cx;
                z = cz;
                forest_found = true;
            }
        }
    }

    Vec3 spawn{x, 30.0f, z};
    const DensitySampler density = sampler_.as_sampler();
    if (const auto ground = raycast_density(density, Vec3{x, 30.0f, z}, Vec3{0.0f, -1.0f, 0.0f}, 80.0f)) {
        spawn.y = ground->y + 0.2f;
    }
    return spawn;
}

void GameServer::tick(Timestep dt) {
    const DensitySampler density = sampler_.as_sampler();
    manager_.update(dt); // advances the day/night clock + (later) the transport objective

    for (const net::ServerEvent& e : server_.poll()) {
        switch (e.type) {
            case net::ServerEventType::ClientConnected: {
                ServerPlayer player;
                player.controller.set_position(spawn_point(e.client));
                players_.emplace(e.client, std::move(player));
                server_.send_welcome(e.client, net::Welcome{e.client, sampler_.seed()});
                ALRYN_INFO("Player {} joined ({} online)", e.client, players_.size());
                break;
            }
            case net::ServerEventType::InputReceived: {
                const auto it = players_.find(e.client);
                if (it == players_.end()) {
                    break;
                }
                // PARRY: raising the shield (block false->true) opens a brief parry window.
                if (e.input.block && !it->second.input.block) {
                    it->second.parry_window = kParryWindow;
                }
                it->second.input = e.input;
                if (e.input.dig) {
                    sampler_.add_edit(e.input.aim, kEditRadius, kEditAmount);
                    server_.broadcast_deform(net::DeformEvent{e.input.aim, kEditRadius, kEditAmount});
                }
                if (e.input.add) {
                    sampler_.add_edit(e.input.aim, kEditRadius, -kEditAmount);
                    server_.broadcast_deform(net::DeformEvent{e.input.aim, kEditRadius, -kEditAmount});
                }
                if (e.input.fire) {
                    const Vec3 eye = it->second.controller.eye_position();
                    Vec3 dir = e.input.aim - eye;
                    if (glm::length(dir) > 0.2f) {
                        dir = glm::normalize(dir);
                        Projectile pr;
                        pr.position = eye + dir * 0.6f;
                        pr.velocity = dir * kProjectileSpeed;
                        pr.owner = e.client;
                        pr.damage = role_stats(it->second.role).ranged_damage;
                        // Projectile per role: Hunter looses an arrow (kind 3), Cleric casts an
                        // arcane bolt (kind 4), everyone else lobs a rock (kind 0).
                        pr.kind = it->second.role == PlayerRole::Hunter  ? 3
                                  : it->second.role == PlayerRole::Cleric ? 4
                                                                          : 0;
                        projectiles_.push_back(pr);
                        if (projectiles_.size() > kMaxProjectiles) {
                            projectiles_.erase(projectiles_.begin());
                        }
                    }
                }
                // Note: melee/build/rally (e.input.attack/build/rally) are part of the
                // dormant siege; see Combat/SiegeMode.cpp. They are ignored here.
                break;
            }
            case net::ServerEventType::ClientDisconnected: {
                players_.erase(e.client);
                server_.broadcast_player_left(e.client);
                ALRYN_INFO("Player {} left ({} online)", e.client, players_.size());
                break;
            }
        }
    }

    // Adopt each player's role (stats + walk speed) and resolve any ability they cast
    // this tick (against the live ambush enemies / allies) before they move.
    update_abilities(dt, density);
    update_spells(dt, density); // Mage combo spells + ageing out raised rock walls

    for (auto& [id, player] : players_) {
        if (riders_.count(id) != 0u || id == pilot_) {
            continue; // riding/driving the wagon - position is set by the contract update
        }
        Vec3 move = player.input.move;
        player.decay_parry(dt.seconds); // the Knight parry window closes quickly after raising the shield
        // DODGE ROLL: a quick burst in a locked direction with i-frames (take_damage evades while
        // roll_timer > 0). Overrides walking / hauling for its brief duration.
        if (player.roll_cd > 0.0f) {
            player.roll_cd -= dt.seconds;
        }
        bool rolling = false;
        if (player.roll_timer > 0.0f) {
            player.roll_timer -= dt.seconds;
            rolling = true;
        } else if (player.input.dodge && player.roll_cd <= 0.0f) {
            Vec3 d{player.input.move.x, 0.0f, player.input.move.z};
            if (glm::length(d) < 0.1f) {
                d = Vec3{std::cos(player.input.yaw), 0.0f, std::sin(player.input.yaw)}; // roll where you face
            }
            player.roll_dir = glm::normalize(d);
            player.roll_timer = kRollDuration;
            player.roll_cd = kRollCooldown;
            rolling = true;
        }
        if (rolling) {
            move = player.roll_dir * kRollBoost; // fast burst, overrides walking / hauling
        } else if (id == tower_ && contract_phase_ == ContractPhase::Active) {
            // Hauling a cart is heavy work: slow the puller by the cart's size + damage, and by how
            // laden the bed is (a full load drags; it lightens + quickens as cargo spills).
            const f32 hf = active_.health / kWagonHealth;
            const f32 f = tow_speed_factor(vehicle_type(active_.type).capacity(), hf) *
                          load_speed_factor(static_cast<u32>(cargo_.size()), active_.goods_total) *
                          rig_speed_mult(rig_level_); // a reinforced rig tows faster (1.0 at stock)
            const f32 l = glm::length(Vec2{move.x, move.z});
            if (l > 1e-3f) {
                move = move / l * f; // exact fraction of walk speed, any direction
            }
        }
        if (collision_) {
            collision_->gather(player.controller.position(), collider_scratch_);
        } else {
            collider_scratch_.clear();
        }
        append_wagon_colliders(collider_scratch_); // can't walk through parked / hauled carts
        // A bridge deck is walkable ground over the river it spans; the active cart's bed is a
        // moving platform you can jump on top of and ride.
        const u32 seed = sampler_.seed();
        player.controller.update(density, move, player.input.jump, dt, collider_scratch_,
                                 [this, seed](f32 x, f32 z) {
                                     return std::max(roads::bridge_height(x, z, seed),
                                                     wagon_top_at(x, z));
                                 });
    }

    // Step thrown rocks: gravity + bounce off terrain/props. With combat dormant they
    // no longer deal damage (the siege resolved hits against enemies/villagers); they
    // just arc and settle.
    for (Projectile& pr : projectiles_) {
        if (collision_) {
            collision_->gather(pr.position, collider_scratch_);
        }
        step_projectile(pr, density, collider_scratch_, dt);
    }
    std::erase_if(projectiles_, [](const Projectile& pr) { return !pr.alive; });

    update_townsfolk(dt, density); // peaceful villagers stroll the nearby towns
    update_contracts(dt, density); // the wagon-transport game loop (offers / haul / ambush)

    net::Snapshot snapshot;
    snapshot.tick = ++tick_;
    snapshot.time_of_day = manager_.time_of_day();
    snapshot.weather = static_cast<u8>(glm::clamp(manager_.weather(), 0.0f, 1.0f) * 255.0f);
    snapshot.money = money_;
    snapshot.delivery_streak = static_cast<u8>(delivery_streak_);
    snapshot.contract_kills = static_cast<u8>(std::min<u32>(contract_kills_, 255u));
    snapshot.rig_level = rig_level_;
    snapshot.contract_phase = static_cast<u8>(contract_phase_);
    snapshot.contract_outcome = contract_outcome_;
    snapshot.players.reserve(players_.size());
    const bool active = contract_phase_ == ContractPhase::Active;
    for (const auto& [id, player] : players_) {
        // Health is broadcast as a 0..100 percent of this player's role max so the bar
        // reads correctly whatever their role's health pool is.
        const u8 hp = static_cast<u8>(
            glm::clamp(player.health / player.max_health, 0.0f, 1.0f) * 100.0f);
        const bool seated = active && (riders_.count(id) != 0u || id == pilot_);
        const f32 yaw = seated ? active_.yaw : player.input.yaw; // seated -> face the vehicle
        // Body action for the animation layer: blocking (held) wins, else a swing while the
        // attack input is held (the client sends it for one frame per click).
        const u8 action = player.roll_timer > 0.0f ? 3u // dodge roll -> client plays it + a dust puff
                          : player.input.block      ? 2u
                          : player.input.attack     ? 1u
                                                    : 0u;
        const u8 shield = static_cast<u8>(
            glm::clamp(player.shield_hp / kAegisAmount, 0.0f, 1.0f) * 255.0f);
        const u8 buffs = static_cast<u8>((player.damage_boost_timer > 0.0f ? 1u : 0u) |
                                         (player.haste_timer > 0.0f ? 2u : 0u));
        snapshot.players.push_back({id, player.controller.position(), yaw, hp, 0,
                                    static_cast<u8>(seated ? 1 : 0),
                                    static_cast<u8>(player.carrying ? 1 : 0),
                                    static_cast<u8>(player.role), player.cast_fx, action, shield,
                                    buffs, player.input.appearance, player.equipment,
                                    player.owned_tier});
    }
    snapshot.projectiles.reserve(projectiles_.size());
    for (const Projectile& pr : projectiles_) {
        snapshot.projectiles.push_back({pr.position, pr.heading, pr.kind});
    }
    snapshot.villagers.reserve(villagers_.size() + 1);
    for (const auto& [id, vg] : villagers_) {
        const u8 shield = static_cast<u8>(glm::clamp(vg.shield_timer / kAegisDuration, 0.0f, 1.0f) * 255.0f);
        snapshot.villagers.push_back({id, vg.position, vg.yaw, 255, vg.kind, shield, vg.appearance});
    }
    // The hired teamster pulling the active wagon rides along as a kind-2 villager.
    if (contract_phase_ == ContractPhase::Active && active_mode_ == WagonMode::Driver && driver_) {
        snapshot.villagers.push_back({driver_->id, driver_->position, driver_->yaw, 255, driver_->kind,
                                      0, driver_->appearance});
    }
    // Ambushers ride in the existing enemy list (rendered red by the client).
    snapshot.enemies.reserve(ambush_.size());
    for (const Enemy& en : ambush_) {
        const u8 hp = static_cast<u8>(
            glm::clamp(en.health / enemy_max_health(en.kind), 0.0f, 1.0f) * 255.0f);
        // Networked action cue for the client (no new wire field - EnemyState.action is reused):
        // brute slam telegraph (2 = winding up) / strike (3 = just slammed), else a melee swing (1).
        u8 action = 0u;
        if (en.kind == 2u) {
            if (en.slam_windup > 0.0f) {
                action = 2u; // winding up -> client draws the danger ring
            } else if (en.attack_cd > kSlamCooldown - 0.18f) {
                action = 3u; // just slammed -> client draws the shockwave
            }
        } else if (en.kind == 3u) {
            if (en.slam_windup > 0.0f) {
                action = 2u; // archer aiming -> client draws a charging glow on the bow
            }
        } else if (en.attack_cd > kEnemyAttackInterval - 0.18f) {
            action = 1u; // melee swing in sync with the hit
        }
        snapshot.enemies.push_back({en.id, en.position, en.yaw, en.kind, hp, action});
    }
    // Wagons: the parked offers while choosing, or the single active cargo en route.
    auto vote_count = [&](u32 wagon_id) {
        u8 n = 0;
        for (const auto& [pid, v] : votes_) {
            if (v.first == wagon_id) {
                ++n;
            }
        }
        return n;
    };
    auto wagon_state = [&](const Wagon& w, WagonMode mode, u8 votes, bool with_horse) {
        net::WagonState s;
        s.id = w.id;
        s.position = w.position;
        s.yaw = w.yaw;
        s.dest = Vec3{w.dest.x, 0.0f, w.dest.y};
        s.source = Vec3{w.source.x, 0.0f, w.source.y};
        s.reward = w.reward;
        s.type = w.type;
        s.health = static_cast<u8>(glm::clamp(w.health / kWagonHealth, 0.0f, 1.0f) * 255.0f);
        s.mode = static_cast<u8>(mode);
        s.difficulty = w.difficulty;
        s.votes = votes;
        s.goods_aboard = static_cast<u8>(cargo_.size()); // crates still in the bed
        s.goods_total = w.goods_total;
        if (with_horse) {
            s.has_horse = 1;
            s.horse_pos = horse_pos_;
            s.horse_yaw = horse_yaw_;
        }
        // Wheel-breakdown state only applies to the active cart.
        if (contract_phase_ != ContractPhase::Offer && w.id == active_.id && wheel_off_) {
            s.wheel_off = 1;
            s.wheel_index = wheel_index_;
            s.wheel_pos = wheel_pos_;
            s.repair = static_cast<u8>(glm::clamp(wheel_repair_, 0.0f, 1.0f) * 255.0f);
        }
        return s;
    };
    if (contract_phase_ == ContractPhase::Offer) {
        snapshot.wagons.reserve(offers_.size());
        for (const Wagon& w : offers_) {
            snapshot.wagons.push_back(wagon_state(w, WagonMode::Parked, vote_count(w.id), false));
        }
    } else { // Active / Settle: just the cargo
        snapshot.wagons.push_back(wagon_state(active_, active_mode_, 0, has_horse_));
    }
    // Cargo crates: the ones in the bed ride as LOCAL positions (the client places them via
    // the cart transform so they slide + tilt with it); the ones that fell out ride as WORLD
    // positions and are pickable.
    snapshot.goods.reserve(cargo_.size() + goods_.size());
    if (contract_phase_ == ContractPhase::Active) {
        const f32 floor = vehicle_type(active_.type).bed().lo.y;
        for (const CargoBox& c : cargo_) {
            // y carries the crate's height in the bed (floor + bump lift) so it visibly bounces.
            snapshot.goods.push_back({c.id, Vec3{c.local.x, floor + c.h, c.local.y}, 0});
        }
    }
    for (const GroundGood& g : goods_) {
        snapshot.goods.push_back({g.id, g.position, 1});
    }
    snapshot.auras.reserve(auras_.size());
    for (const Aura& a : auras_) {
        snapshot.auras.push_back({a.position, a.radius, a.kind});
    }
    snapshot.walls.reserve(walls_.size());
    for (const Wall& w : walls_) {
        snapshot.walls.push_back(
            {w.position, w.yaw, w.length,
             static_cast<u8>(glm::clamp(w.health / kRockWallHealth, 0.0f, 1.0f) * 255.0f)});
    }
    // fires / barricades stay empty (siege dormant); outcome/phase/wave keep defaults.
    server_.broadcast_snapshot(snapshot);
}

// Peaceful townsfolk: ensure one villager per cottage in towns near a player, then have
// each stroll between random spots in the plaza, pausing at each. No combat - the full
// flee/guard/firefight AI lives (dormant) in Combat/SiegeMode.cpp.
void GameServer::update_townsfolk(Timestep dt, const DensitySampler& density) {
    const u32 seed = sampler_.seed();

    for (const auto& [pid, player] : players_) {
        const Vec3 focus = player.controller.position();
        const int vcx = static_cast<int>(std::floor(focus.x / worldgen::village_cell));
        const int vcz = static_cast<int>(std::floor(focus.z / worldgen::village_cell));
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto vv = worldgen::village_at(vcx + dx, vcz + dz, seed);
                if (!vv) {
                    continue;
                }
                u32 hi = 0;
                const auto vgates = detail::village_gate_points(*vv, seed);
                detail::for_each_house(*vv, seed, vgates, [&](const detail::HousePlot& h) {
                    const u32 id = (vv->vseed * 2654435761u) ^ ((hi + 1u) * 40499u);
                    ++hi;
                    if (villagers_.count(id) != 0u) {
                        return;
                    }
                    // Stand them on the street just in front of their house.
                    Vec2 toward = vv->center - h.pos;
                    const f32 len = glm::length(toward);
                    toward = len > 1e-3f ? toward / len : Vec2{0.0f, 1.0f};
                    const Vec2 sp = h.pos + toward * (detail::house_reach(h.variant) + 1.0f);
                    Villager vg;
                    vg.id = id;
                    vg.appearance = villager_look(id);
                    vg.position = Vec3{sp.x, worldgen::height(sp.x, sp.y, seed), sp.y};
                    vg.target = vg.position;
                    vg.home_center = vv->center;
                    vg.home_half = vv->half;
                    vg.rng = id | 1u;
                    villagers_.emplace(id, std::move(vg));
                });

                // Garrison: ~half the towns post a few archer guards on their walls.
                if (detail::hash01(detail::tree_hash(static_cast<int>(vv->vseed), 7, 5151u)) < 0.5f) {
                    for (int gidx = 0; gidx < kWallGuardsPerTown; ++gidx) {
                        const u32 gid = (vv->vseed * 2654435761u) ^
                                        ((static_cast<u32>(gidx) + 1u) * 26171u) ^ 0xA5A50000u;
                        if (villagers_.count(gid) != 0u) {
                            continue;
                        }
                        f32 ang = TwoPi * (static_cast<f32>(gidx) + 0.5f) /
                                      static_cast<f32>(kWallGuardsPerTown) +
                                  detail::hash01(detail::tree_hash(static_cast<int>(vv->vseed), gidx,
                                                                   5152u)) * 0.6f;
                        for (const detail::VillageGate& g : vgates) {
                            if (std::abs(detail::ang_diff(ang, g.ang)) < 0.28f) {
                                ang += 0.45f; // nudge off a gate so the guard stands on solid wall
                            }
                        }
                        const Vec2 bp = detail::town_boundary(*vv, ang, seed);
                        Villager vg;
                        vg.id = gid;
                        vg.kind = 2; // wall archer
                        vg.appearance = villager_look(gid);
                        vg.position = Vec3{bp.x, worldgen::height(bp.x, bp.y, seed) + 2.0f, bp.y};
                        vg.target = vg.position;
                        vg.home_center = vv->center;
                        vg.home_half = vv->half;
                        vg.yaw = ang; // facing outward over the wall
                        vg.rng = gid | 1u;
                        villagers_.emplace(gid, std::move(vg));
                    }
                }
            }
        }
    }

    auto rnd = [](u32& s) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<f32>(s & 0xFFFFFFu) / static_cast<f32>(0xFFFFFFu);
    };
    auto nearest_player = [&](const Vec3& p) {
        f32 best = 1e30f;
        for (const auto& [pid, player] : players_) {
            best = std::min(best, glm::length(player.controller.position() - p));
        }
        return best;
    };
    // So townsfolk crossing a road bridge walk over the deck rather than dropping into the river.
    const std::function<f32(f32, f32)> bridge = [seed](f32 x, f32 z) {
        return roads::bridge_height(x, z, seed);
    };

    for (auto it = villagers_.begin(); it != villagers_.end();) {
        Villager& vg = it->second;
        if (nearest_player(vg.position) > 120.0f) {
            it = villagers_.erase(it); // out of range (respawns deterministically on return)
            continue;
        }
        if (vg.kind == 2) {
            // Wall archer: stand watch and loose a friendly arrow at the nearest ambusher in
            // range (the existing ambush resolution applies the damage). Never moves.
            if (vg.attack_cd > 0.0f) {
                vg.attack_cd -= dt.seconds;
            }
            vg.speed = 0.0f;
            const Enemy* tgt = nullptr;
            f32 best = kWallGuardRange;
            for (const Enemy& e : ambush_) {
                const f32 d = glm::length(e.position - vg.position);
                if (d < best) {
                    best = d;
                    tgt = &e;
                }
            }
            if (tgt != nullptr) {
                Vec3 face = tgt->position - vg.position;
                face.y = 0.0f;
                if (glm::length(face) > 1e-3f) {
                    vg.yaw = std::atan2(face.z, face.x);
                }
                if (vg.attack_cd <= 0.0f) {
                    const Vec3 from = vg.position + Vec3{0.0f, 1.4f, 0.0f};
                    const Vec3 dir = (tgt->position + Vec3{0.0f, 0.7f, 0.0f}) - from;
                    if (glm::length(dir) > 0.3f) {
                        Projectile arrow;
                        arrow.position = from + glm::normalize(dir) * 0.6f;
                        arrow.velocity = glm::normalize(dir) * kWallGuardArrowSpeed;
                        arrow.kind = 3;        // friendly arrow (client renders a tan bolt)
                        arrow.hostile = false; // hurts the ambushers, not the town
                        arrow.damage = kWallGuardArrowDamage;
                        arrow.radius = 0.15f;
                        arrow.life = 3.0f;
                        projectiles_.push_back(arrow);
                        vg.attack_cd = kWallGuardInterval;
                    }
                }
            }
            ++it;
            continue;
        }
        if (collision_) {
            collision_->gather(vg.position, collider_scratch_);
        }
        Vec3 to = vg.target - vg.position;
        to.y = 0.0f;
        vg.wait -= dt.seconds; // doubles as a "give up if stuck" pursuit timer
        if (glm::length(to) < 0.5f || vg.wait <= 0.0f) {
            // (Re)choose a nearby OPEN spot to stroll to. Picking anywhere across the whole town (the
            // old behaviour) aimed villagers at points inside/behind houses, so they just pressed into
            // a wall (step_villager only pushes out of colliders - it can't path around). Validating the
            // target (a few metres away, inside the town, off the roads, not inside a building/prop) and
            // giving up when stuck keeps them strolling the open streets instead of walking into houses.
            auto blocked = [&](const Vec2& p) {
                if (glm::length(p - vg.home_center) > vg.home_half * 0.85f) {
                    return true; // outside the town
                }
                if (glm::length(p - vg.home_center) < detail::kMarketHalf + 1.0f) {
                    return true; // keep out of the central market plaza (don't path into the stalls)
                }
                if (roads::distance(p.x, p.y, seed) < roads::road_half_width) {
                    return true; // on a road / street
                }
                for (const Collider& c : collider_scratch_) {
                    if (glm::length(resolve_collider(c, p, kVillagerRadius, vg.position.y,
                                                     kEnemyHeight) -
                                    p) > 1e-3f) {
                        return true; // inside a building / prop
                    }
                }
                return false;
            };
            Vec2 best{vg.position.x, vg.position.z}; // default: stay put if nowhere open is found
            for (int attempt = 0; attempt < 8; ++attempt) {
                const f32 ang = rnd(vg.rng) * TwoPi;
                const f32 rad = 2.5f + rnd(vg.rng) * 6.0f;
                const Vec2 cand{vg.position.x + std::cos(ang) * rad, vg.position.z + std::sin(ang) * rad};
                if (!blocked(cand)) {
                    best = cand;
                    break;
                }
            }
            vg.target = Vec3{best.x, vg.position.y, best.y};
            vg.wait = 2.5f + rnd(vg.rng) * 4.0f;
        }
        if (glm::length(Vec2{vg.target.x - vg.position.x, vg.target.z - vg.position.z}) >= 0.5f) {
            step_villager(vg, density, collider_scratch_, vg.target, dt, kVillagerSpeed, bridge);
        } else {
            vg.speed = 0.0f;
        }
        ++it;
    }
}

} // namespace alryn
