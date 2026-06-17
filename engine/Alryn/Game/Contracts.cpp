// The wagon-transport contract loop - the active game mode. Implemented as GameServer
// methods (like the dormant siege in Combat/SiegeMode.cpp) so it can reuse the server's
// players_/projectiles_/collision_ directly. Phases: Offer (wagons offered, players vote)
// -> Active (drive/haul the cargo while ambushers attack) -> Settle (delivered/wrecked
// banner) -> Offer again at the new town. See Game/Contract.h for the data + formulas.
#include <Alryn/Net/GameServer.h>

#include <Alryn/Core/Density.h>
#include <Alryn/Core/Log.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/ScatterHash.h>
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/World/Village.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <queue>
#include <span>
#include <utility>

namespace alryn {

namespace {
constexpr f32 kAmbushSpawnRadius = 18.0f; // how far around the wagon ambushers appear
constexpr f32 kAggroRadius = 16.0f;       // an ambusher chases a player within this range
constexpr f32 kArcherShootRange = 16.0f;
constexpr f32 kArcherKeepDist = 9.0f;
constexpr f32 kArcherInterval = 2.4f;
constexpr f32 kArrowSpeed = 20.0f;
constexpr f32 kArrowDamage = 8.0f;

// Ground height under (x,z) via the density function.
f32 ground_at(const DensitySampler& density, f32 x, f32 z) {
    if (const auto g = raycast_density(density, Vec3{x, 60.0f, z}, Vec3{0.0f, -1.0f, 0.0f}, 120.0f)) {
        return g->y;
    }
    return worldgen::height(x, z, 0u);
}

// 8-connected A* over a local 1 m grid that routes from `start` to `goal` (world xz)
// AROUND the colliders (houses / fences / rocks / logs). Returns the path as world-xz
// waypoints; falls back to a straight line to `goal` if no path is found. Bounded grid +
// iteration cap, recomputed only every so often, so it's cheap to run per tick.
std::vector<Vec2> astar_path(const Vec2& start, const Vec2& goal, std::span<const Collider> cols,
                             f32 radius, f32 foot) {
    constexpr f32 cell = 1.0f;
    constexpr int kMaxDim = 46;
    const Vec2 lo = glm::min(start, goal) - Vec2{6.0f, 6.0f};
    const Vec2 hi = glm::max(start, goal) + Vec2{6.0f, 6.0f};
    const int nx = glm::clamp(static_cast<int>((hi.x - lo.x) / cell) + 1, 2, kMaxDim);
    const int nz = glm::clamp(static_cast<int>((hi.y - lo.y) / cell) + 1, 2, kMaxDim);
    auto pos_of = [&](int ix, int iz) {
        return Vec2{lo.x + (static_cast<f32>(ix) + 0.5f) * cell, lo.y + (static_cast<f32>(iz) + 0.5f) * cell};
    };
    auto cell_of = [&](const Vec2& p) {
        return std::pair<int, int>{glm::clamp(static_cast<int>((p.x - lo.x) / cell), 0, nx - 1),
                                   glm::clamp(static_cast<int>((p.y - lo.y) / cell), 0, nz - 1)};
    };
    // Precompute blocked cells (a probe at the cell is pushed by some collider).
    std::vector<char> block(static_cast<usize>(nx) * nz, 0);
    for (int iz = 0; iz < nz; ++iz) {
        for (int ix = 0; ix < nx; ++ix) {
            const Vec2 p = pos_of(ix, iz);
            for (const Collider& c : cols) {
                if (glm::length(resolve_collider(c, p, radius, foot, 1.7f) - p) > 1e-3f) {
                    block[static_cast<usize>(iz) * nx + ix] = 1;
                    break;
                }
            }
        }
    }
    const auto [sx, sz] = cell_of(start);
    const auto [gx, gz] = cell_of(goal);
    auto idx = [&](int ix, int iz) { return static_cast<usize>(iz) * nx + ix; };
    auto heur = [&](int ix, int iz) {
        return std::hypot(static_cast<f32>(ix - gx), static_cast<f32>(iz - gz));
    };
    std::vector<f32> g(static_cast<usize>(nx) * nz, 1e30f);
    std::vector<int> came(static_cast<usize>(nx) * nz, -1);
    std::vector<char> closed(static_cast<usize>(nx) * nz, 0);
    using Node = std::pair<f32, int>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    g[idx(sx, sz)] = 0.0f;
    open.push({heur(sx, sz), static_cast<int>(idx(sx, sz))});
    constexpr int dx8[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    constexpr int dz8[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    bool reached = (sx == gx && sz == gz);
    int iters = 0;
    while (!open.empty() && iters++ < 4000) {
        const int cur = open.top().second;
        open.pop();
        if (closed[cur]) {
            continue;
        }
        closed[cur] = 1;
        const int cix = cur % nx;
        const int ciz = cur / nx;
        if (cix == gx && ciz == gz) {
            reached = true;
            break;
        }
        for (int k = 0; k < 8; ++k) {
            const int ax = cix + dx8[k];
            const int az = ciz + dz8[k];
            if (ax < 0 || az < 0 || ax >= nx || az >= nz || block[idx(ax, az)]) {
                continue;
            }
            if (dx8[k] != 0 && dz8[k] != 0 &&
                (block[idx(cix + dx8[k], ciz)] || block[idx(cix, ciz + dz8[k])])) {
                continue; // don't cut diagonally through a corner
            }
            const f32 step = (dx8[k] != 0 && dz8[k] != 0) ? 1.41421f : 1.0f;
            const f32 ng = g[cur] + step;
            if (ng < g[idx(ax, az)]) {
                g[idx(ax, az)] = ng;
                came[idx(ax, az)] = cur;
                open.push({ng + heur(ax, az), static_cast<int>(idx(ax, az))});
            }
        }
    }
    if (!reached) {
        return {goal}; // no route found - head straight and let towing sort it out
    }
    std::vector<Vec2> path;
    for (int c = static_cast<int>(idx(gx, gz)); c != -1; c = came[c]) {
        path.push_back(pos_of(c % nx, c / nx));
    }
    std::reverse(path.begin(), path.end());
    path.push_back(goal); // finish exactly on the waypoint

    // String-pull: collapse the 1 m grid staircase into a few straight waypoints (keep a
    // node only when there's no clear line of sight to a later one). This removes the
    // node-to-node jitter so the teamster walks smoothly.
    auto los = [&](const Vec2& a, const Vec2& b) {
        const f32 len = glm::length(b - a);
        const int steps = std::max(1, static_cast<int>(len / 0.5f));
        for (int k = 1; k < steps; ++k) {
            const Vec2 p = glm::mix(a, b, static_cast<f32>(k) / static_cast<f32>(steps));
            const auto [ix, iz] = cell_of(p);
            if (block[idx(ix, iz)]) {
                return false;
            }
        }
        return true;
    };
    std::vector<Vec2> simp;
    simp.push_back(path.front());
    usize i = 0;
    while (i + 1 < path.size()) {
        usize j = path.size() - 1;
        for (; j > i + 1; --j) {
            if (los(path[i], path[j])) {
                break;
            }
        }
        simp.push_back(path[j]);
        i = j;
    }
    return simp;
}

// The town nearest a point (scanning the 3x3 village cells around it), if any.
std::optional<worldgen::Village> nearest_town(const Vec3& p, u32 seed) {
    const int cx = static_cast<int>(std::floor(p.x / worldgen::village_cell));
    const int cz = static_cast<int>(std::floor(p.z / worldgen::village_cell));
    std::optional<worldgen::Village> best;
    f32 best_d = 1e30f;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (const auto v = worldgen::village_at(cx + dx, cz + dz, seed)) {
                const f32 d = glm::length(v->center - Vec2{p.x, p.z});
                if (d < best_d) {
                    best_d = d;
                    best = v;
                }
            }
        }
    }
    return best;
}
} // namespace

void GameServer::update_contracts(Timestep dt, const DensitySampler& density) {
    if (players_.empty()) {
        return;
    }
    switch (contract_phase_) {
        case ContractPhase::Offer: {
            // (Re)generate offers when the players' nearest town changes.
            Vec3 centroid{0.0f};
            for (const auto& [id, pl] : players_) {
                centroid += pl.controller.position();
            }
            centroid /= static_cast<f32>(players_.size());
            const auto town = nearest_town(centroid, sampler_.seed());
            if (!town) {
                offers_.clear();
                offer_town_vseed_ = 0;
                break;
            }
            if (town->vseed != offer_town_vseed_ || offers_.empty()) {
                generate_offers();
            }

            // Tally votes: a contract starts only when ALL players pick the same wagon +
            // mode (solo = instant). Record each vote for the snapshot's per-offer count.
            votes_.clear();
            bool all = true;
            bool any = false;
            u32 wid = 0;
            u8 mode = 0;
            for (const auto& [id, pl] : players_) {
                const net::PlayerInput& in = pl.input;
                votes_[id] = {in.vote_wagon, in.vote_mode};
                if (in.vote_wagon == 0 || in.vote_mode == 0) {
                    all = false;
                    continue;
                }
                if (!any) {
                    wid = in.vote_wagon;
                    mode = in.vote_mode;
                    any = true;
                } else if (in.vote_wagon != wid || in.vote_mode != mode) {
                    all = false;
                }
            }
            if (all && any) {
                for (const Wagon& off : offers_) {
                    if (off.id == wid) {
                        accept_contract(off, mode == 2 ? WagonMode::Manual : WagonMode::Driver);
                        break;
                    }
                }
            }
            break;
        }
        case ContractPhase::Active: {
            // `E` (grab) near the active wagon: dismount if riding, unhitch if hauling, else
            // start hauling (manual + no hauler yet) or hop on the back to ride along.
            for (const auto& [id, pl] : players_) {
                if (!pl.input.grab) {
                    continue;
                }
                if (riders_.count(id) != 0u) {
                    riders_.erase(id);
                } else if (tower_ == id) {
                    tower_ = 0;
                } else if (glm::length(pl.controller.position() - active_.position) <
                           kWagonGrabRange + 1.0f) {
                    if (active_mode_ == WagonMode::Manual && tower_ == 0) {
                        tower_ = id; // take the handles
                    } else {
                        riders_.insert(id); // sit on the back
                    }
                }
            }
            if (tower_ != 0 && players_.count(tower_) == 0u) {
                tower_ = 0; // hauler left
            }
            update_wagon(dt, density);
            if (contract_phase_ == ContractPhase::Active) {
                update_ambush(dt, density);
            }
            // Seat the riders on the back of the cart (after it has moved this tick).
            for (const net::PlayerId rid : riders_) {
                const auto it = players_.find(rid);
                if (it == players_.end()) {
                    continue;
                }
                const Vec3 fwd{std::cos(active_.yaw), 0.0f, std::sin(active_.yaw)};
                const Vec3 seat = active_.position - fwd * 0.5f + Vec3{0.0f, 0.85f, 0.0f};
                it->second.controller.set_position(seat);
            }
            break;
        }
        case ContractPhase::Settle: {
            settle_timer_ -= dt.seconds;
            if (settle_timer_ <= 0.0f) {
                contract_outcome_ = 0;
                contract_phase_ = ContractPhase::Offer;
                offers_.clear();
                offer_town_vseed_ = 0; // force a fresh offer at the current town
            }
            break;
        }
    }
}

void GameServer::generate_offers() {
    offers_.clear();
    votes_.clear();
    const u32 seed = sampler_.seed();

    Vec3 centroid{0.0f};
    for (const auto& [id, pl] : players_) {
        centroid += pl.controller.position();
    }
    centroid /= static_cast<f32>(players_.size());
    const auto origin = nearest_town(centroid, seed);
    if (!origin) {
        offer_town_vseed_ = 0;
        return;
    }
    offer_town_vseed_ = origin->vseed;

    // Reserved depot spots (clear of houses/market), one per offer - so a wagon never spawns
    // inside a building.
    const std::vector<Vec3> spots = village_wagon_spots(*origin, seed);

    int idx = 0;
    for (int dz = -roads::road_max_cells; dz <= roads::road_max_cells && idx < static_cast<int>(kMaxOffers); ++dz) {
        for (int dx = -roads::road_max_cells; dx <= roads::road_max_cells && idx < static_cast<int>(kMaxOffers); ++dx) {
            if (dx == 0 && dz == 0) {
                continue;
            }
            const int ocx = static_cast<int>(std::floor(origin->center.x / worldgen::village_cell));
            const int ocz = static_cast<int>(std::floor(origin->center.y / worldgen::village_cell));
            const auto dest = worldgen::village_at(ocx + dx, ocz + dz, seed);
            if (!dest) {
                continue;
            }
            std::vector<Vec2> route = roads::route_polyline(origin->center, dest->center, seed);
            if (route.empty()) {
                continue; // no road links these towns
            }
            const f32 dist = glm::length(dest->center - origin->center);
            const u8 difficulty = static_cast<u8>(
                1u + detail::tree_hash(static_cast<int>(origin->vseed), static_cast<int>(dest->vseed),
                                       5150u) % 3u);
            Wagon wg;
            wg.id = detail::tree_hash(static_cast<int>(origin->vseed), static_cast<int>(dest->vseed),
                                      6161u) | 1u;
            wg.source = origin->center;
            wg.dest = dest->center;
            wg.source_half = origin->half;
            wg.difficulty = difficulty;
            wg.reward = contract_reward(dist, difficulty, false);
            wg.route = std::move(route);
            // Park the offer in a reserved depot spot (clear of buildings); face the dest.
            const Vec3 sp = spots[static_cast<usize>(idx) % spots.size()];
            const Vec2 dir = glm::normalize(dest->center - origin->center);
            wg.position = sp;
            wg.yaw = std::atan2(dir.y, dir.x);
            offers_.push_back(std::move(wg));
            ++idx;
        }
    }
    ALRYN_INFO("Town {} offers {} wagon contract(s)", origin->vseed, offers_.size());
}

void GameServer::accept_contract(const Wagon& chosen, WagonMode mode) {
    const u32 seed = sampler_.seed();
    active_ = chosen;
    if (active_.route.empty()) {
        active_.route = roads::route_polyline(active_.source, active_.dest, seed);
    }
    active_.health = kWagonHealth;
    active_.ambush_waves_spawned = 0;
    active_mode_ = mode;
    tower_ = 0;
    ambush_.clear();
    offers_.clear();
    votes_.clear();

    // The wagon stays at the spot it was parked in; the journey just begins from there.
    // (Earlier it teleported to the route edge, which looked like it vanished.) The driver
    // then heads for the first route point clear of the plaza, out the gate.
    active_.position = chosen.position;
    int start = 0;
    for (usize i = 0; i < active_.route.size(); ++i) {
        if (glm::length(active_.route[i] - active_.source) > 9.0f) {
            start = static_cast<int>(i);
            break;
        }
    }
    active_.progress = static_cast<f32>(start);

    // Hire a teamster NPC to pull the cart in driver mode (it walks the route, the wagon
    // trails it). Manual mode has no driver - a player hauls it instead.
    driver_.reset();
    driver_path_.clear();
    driver_path_i_ = 0;
    driver_repath_ = 0.0f;
    riders_.clear();
    if (mode == WagonMode::Driver) {
        Villager d;
        d.id = active_.id ^ 0xD1234u;
        d.kind = 2; // teamster
        d.appearance = villager_look(d.id);
        const Vec2 dir = glm::normalize(active_.dest - active_.source);
        const Vec2 ahead{active_.position.x + dir.x * kHitchDist, active_.position.z + dir.y * kHitchDist};
        d.position = Vec3{ahead.x, worldgen::height(ahead.x, ahead.y, seed), ahead.y};
        d.yaw = active_.yaw;
        d.home_center = active_.source;
        d.rng = d.id | 1u;
        driver_ = d;
    }
    contract_phase_ = ContractPhase::Active;
    ALRYN_INFO("Contract accepted ({}), reward {}", mode == WagonMode::Manual ? "manual" : "driver",
               active_.reward);
}

void GameServer::update_wagon(Timestep dt, const DensitySampler& density) {
    Wagon& w = active_;
    // The wagon is towed: a puller (the hired teamster NPC, or the hauling player) leads
    // and the cart trails behind it. The wheels spin client-side from the cart's motion.
    Vec3 puller{0.0f};
    f32 speed = 0.0f;
    bool towed = false;

    if (active_mode_ == WagonMode::Driver && driver_) {
        // Walk the teamster toward the next route waypoint, A*-routing AROUND obstacles
        // (houses / fences / rocks / logs) so it never gets stuck; the wagon follows it.
        const Vec2 dxz{driver_->position.x, driver_->position.z};
        const int idx = static_cast<int>(w.progress);
        const Vec2 wp = idx < static_cast<int>(w.route.size()) ? w.route[idx] : w.dest;
        driver_repath_ -= dt.seconds;
        if (driver_path_.empty() || driver_path_i_ >= driver_path_.size() || driver_repath_ <= 0.0f) {
            if (collision_) {
                collision_->gather(driver_->position, collider_scratch_);
            }
            // Clearance is the cart's half-width + margin, so the routed path leaves room
            // for the trailing wagon to clear houses/fences/rocks, not just the teamster.
            driver_path_ = astar_path(dxz, wp, collider_scratch_, 1.1f, driver_->position.y);
            driver_path_i_ = 0;
            driver_repath_ = 0.6f;
        }
        const Vec2 node = driver_path_.empty()
                              ? wp
                              : driver_path_[std::min(driver_path_i_, driver_path_.size() - 1)];
        step_villager(*driver_, density, std::span<const Collider>{},
                      Vec3{node.x, driver_->position.y, node.y}, dt, kWagonDriverSpeed);
        if (driver_path_i_ < driver_path_.size() && glm::length(dxz - node) < 0.6f) {
            ++driver_path_i_;
        }
        if (glm::length(dxz - wp) < 1.4f) { // reached the route waypoint
            w.progress += 1.0f;
            driver_path_.clear();
        }
        puller = driver_->position;
        speed = kWagonDriverSpeed * 1.6f;
        towed = true;
    } else if (active_mode_ == WagonMode::Manual && tower_ != 0 && players_.count(tower_) != 0u) {
        puller = players_.at(tower_).controller.position();
        speed = kWagonManualSpeed * 1.6f;
        towed = true;
    }

    if (towed) {
        // Rope/tongue tow: the cart trails directly behind the puller along the line
        // between them, kept at the hitch distance. Following the connecting LINE (not the
        // puller's facing) means it never swings out to the side when the puller turns.
        Vec3 to = puller - w.position;
        to.y = 0.0f;
        const f32 dist = glm::length(to);
        if (dist > 1e-3f) {
            const Vec3 dir = to / dist;
            const f32 over = dist - kHitchDist; // how far past the hitch length we are
            if (over > 0.0f) {
                const f32 stp = std::min(over, speed * dt.seconds);
                w.position.x += dir.x * stp;
                w.position.z += dir.z * stp;
            }
            w.yaw = std::atan2(dir.z, dir.x); // face the puller (tongue points at them)
        }
        w.position.y = ground_at(density, w.position.x, w.position.z);
    }

    // Delivered: reached the destination town.
    if (glm::length(Vec2{w.position.x - w.dest.x, w.position.z - w.dest.y}) < kDeliverRadius) {
        const bool manual = active_mode_ == WagonMode::Manual;
        money_ += manual ? static_cast<u32>(std::lround(w.reward * kManualRewardMult)) : w.reward;
        contract_outcome_ = 1;
        contract_phase_ = ContractPhase::Settle;
        settle_timer_ = kSettleSeconds;
        ambush_.clear();
        tower_ = 0;
        driver_.reset();
        driver_path_.clear();
        riders_.clear();
        ALRYN_INFO("Wagon delivered! Party money now {}", money_);
        return;
    }
    // Wrecked: the ambush destroyed it.
    if (w.health <= 0.0f) {
        contract_outcome_ = 2;
        contract_phase_ = ContractPhase::Settle;
        settle_timer_ = kSettleSeconds;
        ambush_.clear();
        tower_ = 0;
        driver_.reset();
        driver_path_.clear();
        riders_.clear();
        ALRYN_INFO("Wagon was wrecked - contract failed");
    }
}

void GameServer::update_ambush(Timestep dt, const DensitySampler& density) {
    Wagon& w = active_;

    // Spawn ambushers in up to two waves as the wagon travels.
    auto spawn_wave = [&](u32 count) {
        for (u32 k = 0; k < count; ++k) {
            const u32 h = detail::tree_hash(static_cast<int>(w.id), static_cast<int>(next_ambush_id_),
                                            7000u);
            Enemy e;
            e.id = next_ambush_id_++;
            const u32 roll = h % 6u;
            e.kind = roll == 0u ? 2u : roll == 1u ? 3u : 0u; // ~1/6 brute, ~1/6 archer, rest grunts
            e.health = enemy_max_health(e.kind);
            const f32 a = detail::hash01(h) * TwoPi;
            const f32 r = kAmbushSpawnRadius + detail::hash01(h ^ 0x55u) * 6.0f;
            e.position = Vec3{w.position.x + std::cos(a) * r, 0.0f, w.position.z + std::sin(a) * r};
            e.position.y = ground_at(density, e.position.x, e.position.z);
            e.home = w.position;
            ambush_.push_back(e);
        }
    };
    // Hold the ambush until the wagon is actually OUTSIDE the town walls, so raiders never
    // spawn inside the town while it's still parked / crossing / being hitched.
    const bool left_town =
        glm::length(Vec2{w.position.x - w.source.x, w.position.z - w.source.y}) > w.source_half + 4.0f;
    const u32 total = ambush_count(w.difficulty);
    if (w.ambush_waves_spawned == 0) {
        if (!left_town) {
            return; // still in town - no ambush yet
        }
        spawn_wave((total + 1u) / 2u);
        w.ambush_waves_spawned = 1;
    } else if (w.ambush_waves_spawned == 1) {
        const int idx = static_cast<int>(w.progress);
        if (idx > static_cast<int>(w.route.size()) / 2) {
            spawn_wave(total / 2u);
            w.ambush_waves_spawned = 2;
        }
    }

    // Steer ambushers at the nearest player (else the wagon) and have them strike.
    for (Enemy& e : ambush_) {
        if (collision_) {
            collision_->gather(e.position, collider_scratch_);
        }
        Vec3 goal = w.position;
        f32 best = kAggroRadius;
        ServerPlayer* victim = nullptr;
        for (auto& [id, pl] : players_) {
            const f32 d = glm::length(pl.controller.position() - e.position);
            if (d < best) {
                best = d;
                victim = &pl;
                goal = pl.controller.position();
            }
        }

        if (e.kind == 3u) { // archer: kite to range and loose hostile arrows
            const Vec3 target = victim != nullptr ? victim->controller.position() : w.position;
            const f32 td = glm::length(target - e.position);
            Vec3 g = goal;
            if (td < kArcherShootRange) {
                g = e.position;
                if (td < kArcherKeepDist) {
                    Vec3 away = e.position - target;
                    away.y = 0.0f;
                    if (glm::length(away) > 1e-3f) {
                        g = e.position + glm::normalize(away) * 5.0f;
                    }
                }
            }
            step_enemy(e, density, collider_scratch_, g, dt, kEnemySpeed);
            if (e.attack_cd <= 0.0f && td < kArcherShootRange) {
                const Vec3 from = e.position + Vec3{0.0f, 1.3f, 0.0f};
                Vec3 dir = (target + Vec3{0.0f, 0.6f, 0.0f}) - from;
                if (glm::length(dir) > 0.3f) {
                    Projectile arrow;
                    arrow.position = from + glm::normalize(dir) * 0.5f;
                    arrow.velocity = glm::normalize(dir) * kArrowSpeed;
                    arrow.kind = 1;
                    arrow.hostile = true;
                    arrow.radius = 0.15f;
                    arrow.life = 3.0f;
                    projectiles_.push_back(arrow);
                    e.attack_cd = kArcherInterval;
                }
            }
            continue;
        }

        const f32 spd = e.kind == 2u ? kEnemySpeed * 0.62f : kEnemySpeed;
        const f32 dmg = e.kind == 2u ? kEnemyAttackDamage * 2.3f : kEnemyAttackDamage;
        step_enemy(e, density, collider_scratch_, goal, dt, spd);
        if (e.attack_cd <= 0.0f) {
            const f32 reach = kEnemyAttackRange + kEnemyRadius;
            if (victim != nullptr && best < reach) {
                victim->health -= dmg;
                victim->since_hit = 0.0f;
                e.attack_cd = kEnemyAttackInterval;
            } else if (glm::length(w.position - e.position) < reach + 0.7f) {
                w.health -= kWagonDamage;
                e.attack_cd = kEnemyAttackInterval;
            }
        }
    }

    // Thrown rocks hurt ambushers; hostile arrows hurt players.
    for (Projectile& pr : projectiles_) {
        if (pr.hostile) {
            for (auto& [id, pl] : players_) {
                const Vec3 chest = pl.controller.position() + Vec3{0.0f, 0.9f, 0.0f};
                if (glm::length(chest - pr.position) < pr.radius + 0.55f) {
                    pl.health -= kArrowDamage;
                    pl.since_hit = 0.0f;
                    pr.alive = false;
                }
            }
        } else {
            for (Enemy& e : ambush_) {
                const Vec3 chest = e.position + Vec3{0.0f, 0.9f, 0.0f};
                if (glm::length(chest - pr.position) < pr.radius + kEnemyRadius + 0.3f) {
                    e.health -= kThrowDamage;
                    pr.alive = false;
                }
            }
        }
    }

    // Player melee: a swing hits the nearest ambusher in the front cone (rate-limited).
    for (auto& [id, pl] : players_) {
        if (pl.melee_cd > 0.0f) {
            pl.melee_cd -= dt.seconds;
        }
        if (!pl.input.attack || pl.melee_cd > 0.0f) {
            continue;
        }
        const Vec3 origin = pl.controller.position() + Vec3{0.0f, 0.9f, 0.0f};
        Enemy* hit = nullptr;
        f32 best = kMeleeRange + 1.0f;
        for (Enemy& e : ambush_) {
            const Vec3 chest = e.position + Vec3{0.0f, 0.9f, 0.0f};
            if (!in_attack_cone(origin, pl.input.yaw, chest, kMeleeRange, kMeleeConeCos)) {
                continue;
            }
            const f32 d = glm::length(chest - origin);
            if (d < best) {
                best = d;
                hit = &e;
            }
        }
        if (hit != nullptr) {
            hit->health -= kMeleeDamage;
            pl.melee_cd = 0.35f;
        }
    }

    std::erase_if(ambush_, [](const Enemy& e) { return !e.alive || e.health <= 0.0f; });

    // Player health: regen out of combat, respawn at the town on death.
    for (auto& [id, pl] : players_) {
        pl.since_hit += dt.seconds;
        if (pl.health > 0.0f && pl.since_hit > kPlayerRegenDelay) {
            pl.health = std::min(kPlayerMaxHealth, pl.health + kPlayerRegen * dt.seconds);
        }
        if (pl.health <= 0.0f) {
            pl.controller.set_position(spawn_point(id));
            pl.health = kPlayerMaxHealth;
            pl.since_hit = kPlayerRegenDelay;
            ALRYN_INFO("Player {} was slain and respawned", id);
        }
    }
}

} // namespace alryn
