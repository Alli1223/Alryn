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
#include <Alryn/World/VehicleTypes.h>

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
constexpr f32 kDriverStuckSeconds = 2.0f; // no waypoint progress for this long => skip it

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
    path.front() = start; // anchor exactly on the puller (not the snapped cell centre, which
                          // can sit behind it and cause a backward step on every repath)
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
            const VehicleType& vt = vehicle_type(active_.type);
            // `E` (grab) resolves in priority order: load a carried crate, then pick up a fallen
            // crate, then the role controls (drop your role, or near the wagon take the open
            // control / ride as a passenger).
            for (auto& [id, pl] : players_) {
                if (!pl.input.grab) {
                    continue;
                }
                const Vec3 ppos = pl.controller.position();
                // 1) Carrying a crate: load it back into the cart's bed when close enough.
                if (pl.carrying) {
                    if (cargo_.size() < active_.goods_total &&
                        glm::length(ppos - active_.position) < kGoodLoadRange + vt.reach()) {
                        // Drop it onto the nearest open spot on the bed floor.
                        const CargoBed b = vt.bed();
                        cargo_.push_back({next_good_id_++,
                                          Vec2{glm::mix(b.lo.x + kCargoHalf, b.hi.x - kCargoHalf, 0.5f),
                                               glm::mix(b.lo.z + kCargoHalf, b.hi.z - kCargoHalf, 0.5f)},
                                          Vec2{0.0f}});
                        pl.carrying = false;
                    }
                    continue;
                }
                // 2) Pick up the nearest fallen crate within reach.
                {
                    int best = -1;
                    f32 best_d = kGoodPickupRange;
                    for (usize gi = 0; gi < goods_.size(); ++gi) {
                        const f32 d = glm::length(goods_[gi].position - ppos);
                        if (d < best_d) {
                            best_d = d;
                            best = static_cast<int>(gi);
                        }
                    }
                    if (best >= 0) {
                        goods_.erase(goods_.begin() + best);
                        pl.carrying = true;
                        continue;
                    }
                }
                // 4) Role controls.
                if (riders_.count(id) != 0u) {
                    riders_.erase(id);
                } else if (tower_ == id) {
                    tower_ = 0;
                } else if (pilot_ == id) {
                    pilot_ = 0;
                } else if (glm::length(ppos - active_.position) < kWagonGrabRange + vt.reach()) {
                    if (active_mode_ == WagonMode::Manual && vt.horse_drawn() && pilot_ == 0) {
                        pilot_ = id; // climb up top to drive the carriage
                    } else if (active_mode_ == WagonMode::Manual && !vt.horse_drawn() && tower_ == 0) {
                        tower_ = id; // take the handles to haul
                    } else {
                        riders_.insert(id); // ride as a passenger
                    }
                }
            }
            if (tower_ != 0 && players_.count(tower_) == 0u) {
                tower_ = 0;
            }
            if (pilot_ != 0 && players_.count(pilot_) == 0u) {
                pilot_ = 0;
            }
            update_wagon(dt, density);
            if (contract_phase_ == ContractPhase::Active) {
                update_ambush(dt, density);
            }
            seat_occupants(vt); // place riders/pilot/seated-driver on the vehicle (post-move)
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
    // Never offer more wagons than we have distinct depot spots, or two would share a spot.
    const int max_offers = std::min<int>(static_cast<int>(kMaxOffers), static_cast<int>(spots.size()));

    int idx = 0;
    for (int dz = -roads::road_max_cells; dz <= roads::road_max_cells && idx < max_offers; ++dz) {
        for (int dx = -roads::road_max_cells; dx <= roads::road_max_cells && idx < max_offers; ++dx) {
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
            // Vehicle type: longer / harder routes lean toward bigger vehicles (which pay
            // more); deterministic per (origin,dest) so server + client agree.
            const f32 tn = detail::hash01(
                detail::tree_hash(static_cast<int>(origin->vseed), static_cast<int>(dest->vseed), 5160u));
            u8 type = 0;
            if (dist > 300.0f && tn < 0.65f) {
                type = 2; // carriage
            } else if (dist > 200.0f || tn < 0.5f) {
                type = 1; // wagon
            }
            type = static_cast<u8>(std::min<u32>(type, vehicle_type_count() - 1u));
            wg.type = type;
            wg.reward = static_cast<u32>(std::lround(
                contract_reward(dist, difficulty, false) * capacity_reward_mult(vehicle_type(type).capacity())));
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
    active_.goods_total = goods_for_capacity(vehicle_type(active_.type).capacity());
    wagon_prev_pos_ = active_.position;
    wagon_vel_ = Vec2{0.0f};
    wagon_vy_ = 0.0f;
    goods_.clear();
    active_mode_ = mode;
    tower_ = 0;
    for (auto& [pid, pl] : players_) {
        pl.carrying = false;
    }
    // Load the bed: spread the crates across the cargo floor in a tidy grid.
    cargo_.clear();
    {
        const CargoBed b = vehicle_type(active_.type).bed();
        const f32 x0 = b.lo.x + kCargoHalf;
        const f32 x1 = b.hi.x - kCargoHalf;
        const f32 z0 = b.lo.z + kCargoHalf;
        const f32 z1 = b.hi.z - kCargoHalf;
        const int cols = std::max(1, static_cast<int>(active_.goods_total + 1) / 2);
        for (u8 i = 0; i < active_.goods_total; ++i) {
            const int cx = i % cols;
            const int cz = i / cols;
            const f32 fx = cols > 1 ? static_cast<f32>(cx) / static_cast<f32>(cols - 1) : 0.5f;
            const f32 fz = (i / cols) % 2 == 0 && active_.goods_total > 1 ? 0.3f : 0.7f;
            (void)cz;
            cargo_.push_back({next_good_id_++, Vec2{glm::mix(x0, x1, fx), glm::mix(z0, z1, fz)}, Vec2{0.0f}});
        }
    }
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

    driver_.reset();
    driver_path_.clear();
    driver_path_i_ = 0;
    driver_repath_ = 0.0f;
    driver_stuck_ = 0.0f;
    driver_best_dist_ = 1e9f;
    tow_trail_.clear();
    riders_.clear();
    pilot_ = 0;

    const VehicleType& vt = vehicle_type(active_.type);
    has_horse_ = vt.horse_drawn();
    const Vec2 fdir = glm::normalize(active_.dest - active_.source);
    auto make_driver = [&](u8 kind, const Vec3& pos) {
        Villager d;
        d.id = active_.id ^ 0xD1234u;
        d.kind = kind;
        d.appearance = villager_look(d.id);
        d.position = pos;
        d.yaw = active_.yaw;
        d.home_center = active_.source;
        d.rng = d.id | 1u;
        driver_ = d;
    };
    if (has_horse_) {
        // Carriages are horse-drawn: a horse pulls in front (in either mode).
        const Vec2 hp = Vec2{active_.position.x, active_.position.z} + fdir * (vt.reach() + 1.8f);
        horse_pos_ = Vec3{hp.x, worldgen::height(hp.x, hp.y, seed), hp.y};
        horse_yaw_ = active_.yaw;
        if (mode == WagonMode::Driver) {
            make_driver(3, active_.position); // hired driver sits up top (kind 3, seated)
        }
        // Manual carriage: a player drives it from the top (pilot_), assigned on `E`.
    } else if (mode == WagonMode::Driver) {
        // Hand-cart / wagon hire: a teamster walks the route in front, pulling.
        const Vec2 ahead = Vec2{active_.position.x, active_.position.z} + fdir * kHitchDist;
        make_driver(2, Vec3{ahead.x, worldgen::height(ahead.x, ahead.y, seed), ahead.y});
    }
    contract_phase_ = ContractPhase::Active;
    ALRYN_INFO("Contract accepted ({}), reward {}", mode == WagonMode::Manual ? "manual" : "driver",
               active_.reward);
}

void GameServer::update_wagon(Timestep dt, const DensitySampler& density) {
    Wagon& w = active_;
    const VehicleType& vt = vehicle_type(w.type);
    const f32 hitch = vt.horse_drawn() ? vt.reach() + 1.8f : kHitchDist;
    const f32 dmg = damage_speed_factor(w.health / kWagonHealth); // a battered cart moves slower

    // Walks a puller (horse or teamster) toward the next route waypoint, A*-routing around
    // obstacles so it never snags. It targets sparse, string-pulled nodes using the LIVE
    // position, skipping any node already reached (so a fresh path never sends it back toward
    // its own cell centre) and easing the heading so it doesn't snap or stutter per node.
    auto walk_route = [&](Vec3& pos, f32& yaw) {
        const int ridx = static_cast<int>(w.progress);
        const Vec2 wp = ridx < static_cast<int>(w.route.size()) ? w.route[ridx] : w.dest;
        driver_repath_ -= dt.seconds;
        if (driver_path_.empty() || driver_path_i_ >= driver_path_.size() || driver_repath_ <= 0.0f) {
            if (collision_) {
                collision_->gather(pos, collider_scratch_);
            }
            // Clear enough room for the puller AND the trailing cart (its half-width + a
            // margin), so the chosen path actually fits the whole rig, not just the driver.
            driver_path_ = astar_path({pos.x, pos.z}, wp, collider_scratch_, vt.reach() + 0.4f, pos.y);
            driver_path_i_ = 0;
            driver_repath_ = 0.6f;
        }
        // Skip any leading nodes we've effectively reached (look-ahead): keeps the puller
        // aiming at the next *real* node instead of decelerating + pivoting at each one.
        const Vec2 here{pos.x, pos.z};
        while (driver_path_i_ + 1 < driver_path_.size() &&
               glm::length(here - driver_path_[driver_path_i_]) < 0.9f) {
            ++driver_path_i_;
        }
        const Vec2 node = driver_path_.empty()
                              ? wp
                              : driver_path_[std::min(driver_path_i_, driver_path_.size() - 1)];
        const Vec3 to{node.x - pos.x, 0.0f, node.y - pos.z};
        const f32 d = glm::length(to);
        if (d > 1e-3f) {
            const Vec3 dir = to / d;
            const f32 stp = std::min(d, kWagonDriverSpeed * dmg * dt.seconds);
            pos.x += dir.x * stp;
            pos.z += dir.z * stp;
            // Ease the heading toward the travel direction (shortest way round) rather than
            // snapping it, so the horse/teamster turns smoothly.
            const f32 want = std::atan2(dir.z, dir.x);
            f32 dyaw = want - yaw;
            while (dyaw > Pi) dyaw -= TwoPi;
            while (dyaw < -Pi) dyaw += TwoPi;
            yaw += dyaw * std::min(1.0f, dt.seconds * 7.0f);
        }
        pos.y = ground_at(density, pos.x, pos.z);
        // Advance to (or skip) the current route waypoint.
        const f32 dist_wp = glm::length(Vec2{pos.x, pos.z} - wp);
        auto next_waypoint = [&]() {
            w.progress += 1.0f;
            driver_path_.clear();
            driver_path_i_ = 0;
            driver_stuck_ = 0.0f;
            driver_best_dist_ = 1e9f;
        };
        if (dist_wp < 1.4f) {
            next_waypoint(); // reached it
        } else if (dist_wp < driver_best_dist_ - 0.05f) {
            driver_best_dist_ = dist_wp; // still closing in - making progress
            driver_stuck_ = 0.0f;
        } else {
            // Anti-stuck: the puller isn't getting any closer (an obstacle pocket, or a route
            // point behind a wall makes it oscillate forward/backward). After a couple of
            // seconds with no net progress, give up on this waypoint and skip to the next -
            // A* re-routes to it from wherever we are.
            driver_stuck_ += dt.seconds;
            if (driver_stuck_ > kDriverStuckSeconds) {
                ALRYN_INFO("Driver stuck on a waypoint - skipping ahead");
                next_waypoint();
            }
        }
    };

    Vec3 puller{0.0f};
    f32 speed = 0.0f;
    bool towed = false;

    // A hired (NPC) puller must not run off and drag a snagged cart through a wall: if the
    // cart has fallen too far past the hitch (it's stuck / being shoved out of geometry), the
    // puller holds position until the cart - trailing the breadcrumb - catches back up.
    auto cart_lagging = [&]() {
        const f32 d = glm::length(Vec2{w.position.x - puller.x, w.position.z - puller.z});
        return d > hitch + kTowMaxSlack;
    };

    if (vt.horse_drawn() && active_mode_ == WagonMode::Driver) {
        puller = horse_pos_;
        if (!cart_lagging()) {
            walk_route(horse_pos_, horse_yaw_); // a horse pulls the carriage along the route
            puller = horse_pos_;
        }
        speed = kWagonDriverSpeed * 1.6f * dmg;
        towed = true;
    } else if (vt.horse_drawn()) {
        // Manual carriage: the pilot drives it like a car - throttle (W/S) + rein (A/D).
        if (pilot_ != 0 && players_.count(pilot_) != 0u) {
            const net::PlayerInput& in = players_.at(pilot_).input;
            const f32 throttle = glm::clamp(in.throttle, -1.0f, 1.0f);
            const f32 steer = glm::clamp(in.steer, -1.0f, 1.0f);
            w.yaw += steer * kCarriageTurnRate * dt.seconds * (0.35f + 0.65f * std::abs(throttle));
            const Vec3 dir{std::cos(w.yaw), 0.0f, std::sin(w.yaw)};
            Vec2 next{w.position.x + dir.x * throttle * kCarriageSpeed * dmg * dt.seconds,
                      w.position.z + dir.z * throttle * kCarriageSpeed * dmg * dt.seconds};
            if (collision_) {
                collision_->gather(Vec3{next.x, w.position.y, next.y}, collider_scratch_);
                for (const Collider& c : collider_scratch_) {
                    next = resolve_collider(c, next, vt.reach(), w.position.y, 1.5f);
                }
            }
            w.position.x = next.x;
            w.position.z = next.y;
            w.position.y = ground_at(density, w.position.x, w.position.z);
        }
        // The horse rides in front along the carriage heading.
        const Vec3 hdir{std::cos(w.yaw), 0.0f, std::sin(w.yaw)};
        horse_pos_ = w.position + hdir * hitch;
        horse_pos_.y = ground_at(density, horse_pos_.x, horse_pos_.z);
        horse_yaw_ = w.yaw;
    } else if (active_mode_ == WagonMode::Driver && driver_) {
        puller = driver_->position;
        if (!cart_lagging()) {
            walk_route(driver_->position, driver_->yaw); // teamster walks in front, pulling
            puller = driver_->position;
        }
        speed = kWagonDriverSpeed * 1.6f * dmg;
        towed = true;
    } else if (active_mode_ == WagonMode::Manual && tower_ != 0 && players_.count(tower_) != 0u) {
        puller = players_.at(tower_).controller.position();
        speed = kWagonManualSpeed * 1.6f; // the tower's own walk speed already reflects size+damage
        towed = true;
    }

    if (towed) {
        // Breadcrumb tow: the cart trails the puller along the puller's OWN recent path (not
        // the straight line to it), so it rounds the same corners the driver pathfound around
        // - it can't be dragged through a house/wall the driver walked around. The cart sits
        // `hitch` of arc-length back along that trail.
        if (tow_trail_.empty()) {
            tow_trail_.push_back(w.position);
            tow_trail_.push_back(puller);
        } else if (glm::length(Vec2{puller.x - tow_trail_.back().x,
                                    puller.z - tow_trail_.back().z}) > 0.3f) {
            tow_trail_.push_back(puller);
        } else {
            tow_trail_.back() = puller; // keep the head current without bloating the trail
        }
        if (tow_trail_.size() > 64) {
            tow_trail_.erase(tow_trail_.begin(), tow_trail_.begin() + (tow_trail_.size() - 64));
        }

        // Walk `hitch` back from the head (puller) along the trail to find the cart's target.
        Vec2 target{puller.x, puller.z};
        f32 want = hitch;
        for (usize i = tow_trail_.size() - 1; i > 0; --i) {
            const Vec2 a{tow_trail_[i].x, tow_trail_[i].z};
            const Vec2 b{tow_trail_[i - 1].x, tow_trail_[i - 1].z};
            const f32 seg = glm::length(a - b);
            if (seg >= want) {
                target = a + (b - a) * (want / std::max(seg, 1e-4f));
                break;
            }
            want -= seg;
            target = b; // ran off the oldest crumb - sit at the tail
        }

        // Move the cart toward that target (its catch-up speed), turning to face travel.
        Vec2 cart{w.position.x, w.position.z};
        const Vec2 to = target - cart;
        const f32 d = glm::length(to);
        if (d > 1e-3f) {
            const Vec2 dir = to / d;
            cart += dir * std::min(d, speed * dt.seconds);
            w.yaw = std::atan2(dir.y, dir.x);
        }
        // The towed cart is a solid body too: push it out of rocks, fences, logs and town
        // walls instead of sliding through them.
        if (collision_) {
            collision_->gather(w.position, collider_scratch_);
            for (const Collider& c : collider_scratch_) {
                cart = resolve_collider(c, cart, vt.reach(), w.position.y, 1.5f);
            }
        }
        w.position.x = cart.x;
        w.position.z = cart.y;
        w.position.y = ground_at(density, w.position.x, w.position.z);
    }

    // Slide the physical cargo in the bed (and spill it out on bumps) - all driven by the
    // cart's real acceleration + the terrain under the wheels, no cart-tilt model.
    update_cargo(dt, density);

    // Delivered: reached the destination town. Pay scales with the share of the load still in
    // the bed, so crates that bounced out and weren't recovered cost money.
    if (glm::length(Vec2{w.position.x - w.dest.x, w.position.z - w.dest.y}) < kDeliverRadius) {
        const bool manual = active_mode_ == WagonMode::Manual;
        const f32 frac =
            w.goods_total > 0 ? static_cast<f32>(cargo_.size()) / static_cast<f32>(w.goods_total) : 1.0f;
        const f32 base = manual ? static_cast<f32>(w.reward) * kManualRewardMult
                                : static_cast<f32>(w.reward);
        money_ += static_cast<u32>(std::lround(base * frac));
        contract_outcome_ = 1;
        contract_phase_ = ContractPhase::Settle;
        settle_timer_ = kSettleSeconds;
        ALRYN_INFO("Wagon delivered ({}/{} crates)! Party money now {}", cargo_.size(), w.goods_total,
                   money_);
        end_contract_cleanup();
        return;
    }
    // Wrecked: the ambush destroyed it.
    if (w.health <= 0.0f) {
        contract_outcome_ = 2;
        contract_phase_ = ContractPhase::Settle;
        settle_timer_ = kSettleSeconds;
        end_contract_cleanup();
        ALRYN_INFO("Wagon was wrecked - contract failed");
    }
}

// Simulates the cargo crates riding in the bed: each is a little body sliding on the bed
// floor, pushed by the cart's real acceleration (inertia) and by gravity along the
// terrain-tilted bed, damped by friction, bouncing off the bed rails - and tumbling out onto
// the ground when a bump or hard turn throws it over a rail fast enough. No cart-tilt model.
void GameServer::update_cargo(Timestep dt, const DensitySampler& density) {
    Wagon& w = active_;
    const f32 dts = std::max(dt.seconds, 1e-4f);
    // Cart velocity + acceleration this tick (world xz, plus vertical for bump detection).
    const Vec2 pos{w.position.x, w.position.z};
    const Vec2 vel = (pos - Vec2{wagon_prev_pos_.x, wagon_prev_pos_.z}) / dts;
    const Vec2 accel = (vel - wagon_vel_) / dts;
    const f32 cart_vy = (w.position.y - wagon_prev_pos_.y) / dts;
    const f32 cart_vy_accel = (cart_vy - wagon_vy_) / dts;
    wagon_vel_ = vel;
    wagon_vy_ = cart_vy;
    wagon_prev_pos_ = w.position;
    if (cargo_.empty()) {
        return;
    }

    // Bed slope from the terrain under the cart, in the cart's local frame (x = fore/aft,
    // y = lateral). Gravity pulls crates downhill; cart acceleration slings them the other way.
    const Vec2 fwd{std::cos(w.yaw), std::sin(w.yaw)};
    const Vec2 lat{-std::sin(w.yaw), std::cos(w.yaw)};
    constexpr f32 sd = 0.9f;
    auto gnd = [&](const Vec2& o) { return ground_at(density, w.position.x + o.x, w.position.z + o.y); };
    const f32 slope_f = (gnd(fwd * sd) - gnd(fwd * -sd)) / (2.0f * sd);
    const f32 slope_l = (gnd(lat * sd) - gnd(lat * -sd)) / (2.0f * sd);
    const Vec2 grav_local{-slope_f * kCargoGravity, -slope_l * kCargoGravity};
    // Clamp the cart's acceleration so a rope-tow "snap" can't fling crates.
    Vec2 accel_local{glm::dot(accel, fwd), glm::dot(accel, lat)};
    accel_local.x = glm::clamp(accel_local.x, -kCargoMaxAccel, kCargoMaxAccel);
    accel_local.y = glm::clamp(accel_local.y, -kCargoMaxAccel, kCargoMaxAccel);
    // A bump that drops the bed away (cart accelerating downward) tosses crates up off the
    // floor; clamp so a single jolt can't launch them absurdly high.
    const f32 lift = glm::clamp(-cart_vy_accel, 0.0f, 80.0f);

    const CargoBed b = vehicle_type(w.type).bed();
    const f32 x0 = b.lo.x + kCargoHalf;
    const f32 x1 = b.hi.x - kCargoHalf;
    const f32 z0 = b.lo.z + kCargoHalf;
    const f32 z1 = b.hi.z - kCargoHalf;
    const f32 friction = std::max(0.0f, 1.0f - kCargoFriction * dts);

    // 1) Integrate each crate: slide on the floor (gravity-along-bed + inertia, friction) and
    //    rise/fall vertically (a bump's lift vs. gravity), resting back on the bed floor.
    for (CargoBox& c : cargo_) {
        c.vel += (grav_local - accel_local * kCargoInertia) * dts;
        c.vel *= friction;
        c.local += c.vel * dts;
        c.vh += (lift - kCargoVertGravity) * dts;
        c.h += c.vh * dts;
        if (c.h <= 0.0f) {
            c.h = 0.0f;
            c.vh = c.vh < 0.0f ? -c.vh * kCargoFloorBounce : c.vh; // land + small bounce
        }
        c.h = std::min(c.h, kCargoMaxLift);
    }

    // 2) Crate-vs-crate collisions (only when their heights overlap): separate the boxes along
    //    the least-penetrating xz axis and average their velocity there, so they stack/jostle
    //    instead of sliding through one another.
    constexpr f32 full = 2.0f * kCargoHalf;
    for (int it = 0; it < 4; ++it) {
        for (usize i = 0; i < cargo_.size(); ++i) {
            for (usize j = i + 1; j < cargo_.size(); ++j) {
                if (std::abs(cargo_[i].h - cargo_[j].h) >= full) {
                    continue; // stacked at different heights - no xz collision
                }
                const Vec2 d = cargo_[j].local - cargo_[i].local;
                const f32 pen_x = full - std::abs(d.x);
                const f32 pen_z = full - std::abs(d.y);
                if (pen_x <= 0.0f || pen_z <= 0.0f) {
                    continue; // not overlapping
                }
                if (pen_x <= pen_z) {
                    const f32 push = pen_x * 0.5f * (d.x < 0.0f ? -1.0f : 1.0f);
                    cargo_[i].local.x -= push;
                    cargo_[j].local.x += push;
                    const f32 avg = 0.5f * (cargo_[i].vel.x + cargo_[j].vel.x);
                    cargo_[i].vel.x = avg;
                    cargo_[j].vel.x = avg;
                } else {
                    const f32 push = pen_z * 0.5f * (d.y < 0.0f ? -1.0f : 1.0f);
                    cargo_[i].local.y -= push;
                    cargo_[j].local.y += push;
                    const f32 avg = 0.5f * (cargo_[i].vel.y + cargo_[j].vel.y);
                    cargo_[i].vel.y = avg;
                    cargo_[j].vel.y = avg;
                }
            }
        }
    }

    // 3) Walls: a crate below the rail top is contained (clamp + bounce off a SOLID wall); a
    //    crate launched above the rail (h >= wall) by a bump can pass over it and tumble out.
    //    The carriage's very tall wall means its load never clears - the enclosed-cabin benefit.
    auto rail = [&](f32& p, f32& v, f32 lo, f32 hi, bool over) {
        if (p < lo) {
            if (over) {
                return true; // above the wall - slides off over the top
            }
            p = lo;
            v = -v * kCargoRestitution;
        } else if (p > hi) {
            if (over) {
                return true;
            }
            p = hi;
            v = -v * kCargoRestitution;
        }
        return false;
    };
    for (usize i = 0; i < cargo_.size();) {
        CargoBox& c = cargo_[i];
        const bool over = c.h >= b.wall;                          // launched above the rail top
        const bool out_x = rail(c.local.x, c.vel.x, x0, x1, over); // evaluate both rails
        const bool out_z = rail(c.local.y, c.vel.y, z0, z1, over);
        if (out_x || out_z) {
            const Vec2 wp = pos + fwd * c.local.x + lat * c.local.y;
            Vec3 gp{wp.x, 0.0f, wp.y};
            gp.y = ground_at(density, gp.x, gp.z);
            goods_.push_back({c.id, gp});
            cargo_.erase(cargo_.begin() + static_cast<long>(i));
            ALRYN_INFO("A crate bounced up and out over the cart wall!");
        } else {
            ++i;
        }
    }
}

// Box colliders for every wagon (parked offers, or the active cargo) so players can't walk
// through them. The cart mesh is drawn with rotateY(-yaw), so the collider's yaw is +yaw.
void GameServer::append_wagon_colliders(std::vector<Collider>& out) const {
    auto add = [&](const Wagon& wg) {
        const Vec2 fp = vehicle_type(wg.type).footprint();
        Collider c;
        c.shape = Collider::Shape::Box;
        c.center = wg.position;
        c.yaw = wg.yaw;
        c.half = fp;
        c.y_min = wg.position.y;
        c.y_max = wg.position.y + 1.6f;
        out.push_back(c);
    };
    if (contract_phase_ == ContractPhase::Offer) {
        for (const Wagon& o : offers_) {
            add(o);
        }
    } else {
        add(active_);
    }
}

// Shared teardown when a contract ends (delivered or wrecked): clear the haul state.
void GameServer::end_contract_cleanup() {
    ambush_.clear();
    tower_ = 0;
    pilot_ = 0;
    driver_.reset();
    driver_path_.clear();
    tow_trail_.clear();
    riders_.clear();
    has_horse_ = false;
    cargo_.clear();
    goods_.clear();
    for (auto& [pid, pl] : players_) {
        pl.carrying = false;
    }
}

// Places the pilot, passengers and seated NPC driver on the vehicle (after it has moved),
// each at a distinct local seat transformed into the world like the mesh (rotateY(-yaw)).
void GameServer::seat_occupants(const VehicleType& vt) {
    auto seat_world = [&](const Vec3& local) {
        const Vec3 off = Vec3{glm::rotate(Mat4{1.0f}, -active_.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                              Vec4{local, 0.0f}};
        return active_.position + off;
    };
    if (pilot_ != 0 && players_.count(pilot_) != 0u && vt.has_driver_seat()) {
        players_.at(pilot_).controller.set_position(seat_world(vt.driver_seat()));
    }
    if (driver_ && driver_->kind == 3) { // hired driver seated up top
        driver_->position = seat_world(vt.driver_seat());
        driver_->yaw = active_.yaw;
    }
    const std::vector<Seat> seats = vt.seats();
    std::vector<net::PlayerId> rs(riders_.begin(), riders_.end());
    std::sort(rs.begin(), rs.end()); // stable seat assignment by id
    for (usize i = 0; i < rs.size(); ++i) {
        const auto it = players_.find(rs[i]);
        if (it == players_.end()) {
            continue;
        }
        const Vec3 local = seats.empty() ? Vec3{-0.6f, 0.85f, 0.0f} : seats[i % seats.size()].pos;
        it->second.controller.set_position(seat_world(local));
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
