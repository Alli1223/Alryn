#include <Alryn/Net/GameServer.h>

#include <Alryn/Core/Density.h>
#include <Alryn/Core/Log.h>
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

    for (auto& [id, player] : players_) {
        if (riders_.count(id) != 0u || id == pilot_) {
            continue; // riding/driving the wagon - position is set by the contract update
        }
        Vec3 move = player.input.move;
        if (id == tower_ && contract_phase_ == ContractPhase::Active) {
            // Hauling a cart is heavy work: slow the puller by the cart's size + damage.
            const f32 hf = active_.health / kWagonHealth;
            const f32 f = tow_speed_factor(vehicle_type(active_.type).capacity(), hf);
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
        player.controller.update(density, move, player.input.jump, dt, collider_scratch_);
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
    snapshot.money = money_;
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
        const u8 action = player.input.block ? 2u : (player.input.attack ? 1u : 0u);
        const u8 shield = static_cast<u8>(
            glm::clamp(player.shield_hp / kAegisAmount, 0.0f, 1.0f) * 255.0f);
        snapshot.players.push_back({id, player.controller.position(), yaw, hp, 0,
                                    static_cast<u8>(seated ? 1 : 0),
                                    static_cast<u8>(player.carrying ? 1 : 0),
                                    static_cast<u8>(player.role), player.cast_fx, action, shield,
                                    player.input.appearance});
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
        // Flag a swing for the brief window just after the enemy struck (attack_cd was reset),
        // so the client plays the attack animation in sync with the hit.
        const u8 action =
            (en.kind != 3 && en.attack_cd > kEnemyAttackInterval - 0.18f) ? 1u : 0u; // melee only
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

    for (auto it = villagers_.begin(); it != villagers_.end();) {
        Villager& vg = it->second;
        if (nearest_player(vg.position) > 120.0f) {
            it = villagers_.erase(it); // out of range (respawns deterministically on return)
            continue;
        }
        if (collision_) {
            collision_->gather(vg.position, collider_scratch_);
        }
        Vec3 to = vg.target - vg.position;
        to.y = 0.0f;
        if (glm::length(to) < 0.5f) {
            vg.speed = 0.0f;
            vg.wait -= dt.seconds;
            if (vg.wait <= 0.0f) {
                const f32 ang = rnd(vg.rng) * TwoPi;
                const f32 rad = rnd(vg.rng) * vg.home_half * 0.7f;
                vg.target = Vec3{vg.home_center.x + std::cos(ang) * rad, vg.position.y,
                                 vg.home_center.y + std::sin(ang) * rad};
                vg.wait = 1.0f + rnd(vg.rng) * 3.0f; // pause a beat at each stop
            }
        } else {
            step_villager(vg, density, collider_scratch_, vg.target, dt, kVillagerSpeed);
        }
        ++it;
    }
}

} // namespace alryn
