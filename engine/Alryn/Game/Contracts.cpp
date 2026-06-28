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
constexpr f32 kArrowDamage = 8.0f; // fallback for any hostile arrow without an explicit damage
constexpr f32 kDriverStuckSeconds = 2.0f; // no waypoint progress for this long => skip it
constexpr f32 kUnstickSeconds = 2.0f; // cart snagged (tow-gate pinned) this long => pull it free

// Ground height under (x,z) via the density function. With `bridge_seed` set, a bridge deck spanning
// a river here counts as ground (so the wagon + its puller ride over the bridge, not into the river).
f32 ground_at(const DensitySampler& density, f32 x, f32 z, u32 bridge_seed = 0) {
    f32 g = worldgen::height(x, z, 0u);
    if (const auto h = raycast_density(density, Vec3{x, 60.0f, z}, Vec3{0.0f, -1.0f, 0.0f}, 120.0f)) {
        g = h->y;
    }
    if (bridge_seed != 0) {
        g = std::max(g, roads::bridge_height(x, z, bridge_seed));
    }
    return g;
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
                if (wheel_carrier_ == id) {
                    continue; // hands full with the wheel; it refits itself by the cart
                }
                const Vec3 ppos = pl.controller.position();
                // 0) A wheel has come off: pick up the fallen wheel (E within reach), if not already
                // carrying something. Carrying the wheel back to the cart refits it (update_wheel).
                if (wheel_off_ && wheel_carrier_ == 0 && !pl.carrying &&
                    glm::length(ppos - wheel_pos_) < kWheelPickupRange) {
                    wheel_carrier_ = id;
                    continue;
                }
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
                    if (active_mode_ == WagonMode::Driver) {
                        resync_driver_progress(); // hand the cart back to the AI driver from here on
                    }
                } else if (pilot_ == id) {
                    pilot_ = 0;
                } else if (glm::length(ppos - active_.position) < kWagonGrabRange + vt.reach()) {
                    // Taking the handles / driver's seat works in EITHER mode: in Driver mode a
                    // player can grab on to help a hired driver haul a stuck cart free, then let
                    // go (press again) to hand it back.
                    if (vt.horse_drawn() && pilot_ == 0) {
                        pilot_ = id; // climb up top to drive the carriage
                    } else if (!vt.horse_drawn() && tower_ == 0) {
                        tower_ = id; // take the handles to haul (or help the driver)
                    } else {
                        riders_.insert(id); // ride as a passenger
                    }
                }
            }
            if (tower_ != 0 && players_.count(tower_) == 0u) {
                tower_ = 0; // the hauling player vanished - hand the cart back to the AI driver from here
                if (active_mode_ == WagonMode::Driver) {
                    resync_driver_progress();
                }
            }
            if (pilot_ != 0 && players_.count(pilot_) == 0u) {
                pilot_ = 0;
            }
            update_wheel(dt, density);
            const Vec3 cart_before = active_.position;
            update_wagon(dt, density);
            if (contract_phase_ == ContractPhase::Active) {
                // Carry along anyone standing on top of the cart (a moving platform).
                carry_top_riders(Vec2{active_.position.x - cart_before.x,
                                      active_.position.z - cart_before.z}, vt);
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

    // Destinations come from the TOWN GRAPH (roads::reachable_towns), so a contract can target a
    // far town reached by a multi-hop road through other towns - not just an immediate neighbour.
    const std::vector<worldgen::Village> dests = roads::reachable_towns(origin->center, seed, 32);
    const int navail = static_cast<int>(dests.size());
    const int want = std::min(max_offers, navail);
    std::vector<bool> spot_used(spots.size(), false); // each offer claims one gate-aligned depot
    for (int k = 0; k < want; ++k) {
        // Spread the picks across the reachable set (sorted near -> far) so the board always mixes
        // short hops with long multi-hop hauls that thread through other towns + tougher biomes.
        const int di = (navail <= want) ? k : (k * navail) / want;
        const worldgen::Village& dest = dests[static_cast<usize>(di)];
        std::vector<Vec2> route = roads::route_polyline(origin->center, dest.center, seed);
        if (route.empty()) {
            continue; // no road links these towns
        }
        const f32 dist = roads::route_length(route); // the TRUE road length (long for multi-hop)
        // Difficulty (ambush size) comes from the BIOMES the road crosses - a haul over mountain
        // passes / through bogs + deserts is genuinely tougher than a flat forest run - and a long
        // haul is inherently riskier, so it nudges the tier up.
        u8 difficulty = roads::route_difficulty(route, seed);
        if (dist > 420.0f) {
            difficulty = std::min<u8>(3u, static_cast<u8>(difficulty + 1u));
        }
        Wagon wg;
        wg.id = detail::tree_hash(static_cast<int>(origin->vseed), static_cast<int>(dest.vseed),
                                  6161u) | 1u;
        wg.source = origin->center;
        wg.dest = dest.center;
        wg.source_half = origin->half;
        wg.difficulty = difficulty;
        // Vehicle type: longer / harder routes lean toward bigger vehicles (which pay more);
        // deterministic per (origin,dest) so server + client agree.
        const f32 tn = detail::hash01(
            detail::tree_hash(static_cast<int>(origin->vseed), static_cast<int>(dest.vseed), 5160u));
        u8 type = 0;
        if (dist > 300.0f && tn < 0.65f) {
            type = 2; // carriage
        } else if (dist > 200.0f || tn < 0.5f) {
            type = 1; // wagon
        }
        type = static_cast<u8>(std::min<u32>(type, vehicle_type_count() - 1u));
        wg.type = type;
        // A per-offer modifier (hazardous / bulk / safe, deterministic from the id) varies the pay.
        wg.reward = static_cast<u32>(
            std::lround(contract_reward(dist, difficulty, false) *
                        capacity_reward_mult(vehicle_type(type).capacity()) *
                        modifier_effect(contract_modifier(wg.id)).pay_mult));
        // Face along the first leg of the route (the way it leaves town through its gate).
        Vec2 dir = dest.center - origin->center;
        if (route.size() >= 2) {
            dir = route[1] - route[0];
        }
        const f32 dl = glm::length(dir);
        const Vec2 exit_dir = dl > 1e-3f ? dir / dl : Vec2{1.0f, 0.0f};
        // Park the offer at the reserved depot spot best aligned with its OWN exit gate, so the cart
        // sits on the side it leaves from and barely has to cross town to get out (no snaking
        // through buildings). Each offer claims a distinct spot.
        int best_spot = -1;
        f32 best_align = -2.0f;
        for (usize s = 0; s < spots.size(); ++s) {
            if (spot_used[s]) {
                continue;
            }
            const Vec2 sd = Vec2{spots[s].x, spots[s].z} - origin->center;
            const f32 sl = glm::length(sd);
            const f32 align = sl > 1e-3f ? glm::dot(sd / sl, exit_dir) : 0.0f;
            if (align > best_align) {
                best_align = align;
                best_spot = static_cast<int>(s);
            }
        }
        if (best_spot < 0) {
            continue; // no depot spot free
        }
        spot_used[static_cast<usize>(best_spot)] = true;
        wg.position = spots[static_cast<usize>(best_spot)];
        wg.yaw = std::atan2(exit_dir.y, exit_dir.x);
        wg.route = std::move(route);
        offers_.push_back(std::move(wg));
    }
    ALRYN_INFO("Town {} offers {} wagon contract(s)", origin->vseed, offers_.size());
}

void GameServer::accept_contract(const Wagon& chosen, WagonMode mode) {
    const u32 seed = sampler_.seed();
    active_ = chosen;
    if (active_.route.empty()) {
        active_.route = roads::route_polyline(active_.source, active_.dest, seed);
    }
    active_.health = rig_max_health(rig_level_); // a reinforced rig starts (and caps) tougher
    active_.ambush_waves_spawned = 0;
    active_.goods_total = goods_for_capacity(vehicle_type(active_.type).capacity());
    wagon_prev_pos_ = active_.position;
    wagon_vel_ = Vec2{0.0f};
    wagon_vy_ = 0.0f;
    wheel_off_ = false;
    wheel_carrier_ = 0;
    wheel_repair_ = 0.0f;
    bandit_cd_ = 0.0f;
    wheel_break_cd_ = kWheelBreakMinTime + (kWheelBreakAvgTime - kWheelBreakMinTime) *
                                               detail::hash01(active_.id ^ 0x5733EEDu);
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
    // Begin at the first route point that's farther out than the wagon already sits, so the driver
    // tows it FORWARD out the gate from where it's parked (gate-aligned) instead of dragging it
    // back toward the plaza first.
    const f32 wagon_r = glm::length(Vec2{chosen.position.x, chosen.position.z} - active_.source);
    int start = static_cast<int>(active_.route.size()) - 1;
    for (usize i = 0; i < active_.route.size(); ++i) {
        if (glm::length(active_.route[i] - active_.source) > std::max(9.0f, wagon_r + 2.0f)) {
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
    driver_snag_ = 0.0f;
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
    contract_elapsed_ = 0.0f; // start the rush-bonus clock
    contract_kills_ = 0;      // fresh kill-bounty tally for this haul
    contract_downs_ = 0;      // fresh no-casualty (unscathed) tally for this haul
    for (auto& [pid, pl] : players_) {
        pl.used_second_wind = false; // each player gets one clutch second wind per haul
        pl.rampage_stacks = 0;       // fresh kill momentum for the new haul
        pl.rampage_timer = 0.0f;
    }
    ALRYN_INFO("Contract accepted ({}), reward {}", mode == WagonMode::Manual ? "manual" : "driver",
               active_.reward);
}

void GameServer::resync_driver_progress() {
    // A player took the handles, towed the cart (maybe well past several route waypoints), then let go.
    // Resume the hired teamster from the route node NEAREST the cart's CURRENT position - continuing
    // forward from there - instead of marching back to the stale waypoint it was heading for. The
    // cached A* path is dropped so the driver re-routes fresh from where the cart now sits.
    if (active_.route.empty()) {
        return;
    }
    const Vec2 here{active_.position.x, active_.position.z};
    const int besti = nearest_route_index(here, active_.route);
    // Aim at the node just AHEAD of the nearest, so the driver heads onward (not back to a node it has
    // effectively already reached).
    const int last = static_cast<int>(active_.route.size()) - 1;
    active_.progress = static_cast<f32>(besti + 1 < last ? besti + 1 : last);
    driver_path_.clear();
    driver_path_i_ = 0;
    driver_repath_ = 0.0f;
    driver_stuck_ = 0.0f;
    driver_best_dist_ = 1e9f;
}

void GameServer::update_wagon(Timestep dt, const DensitySampler& density) {
    Wagon& w = active_;
    contract_elapsed_ += dt.seconds; // the rush-bonus clock runs the whole haul (even while stranded)
    const u32 wseed = sampler_.seed(); // so the cart + its puller ride bridges over rivers
    // A wheel is off: the cart is stranded - it can't roll until a player refits the wheel. Keep it
    // grounded + zero its velocity (so cargo settles), but don't tow or advance it.
    if (wheel_off_) {
        w.position.y = ground_at(density, w.position.x, w.position.z, wseed);
        wagon_vel_ = Vec2{0.0f};
        wagon_prev_pos_ = w.position;
        seat_occupants(vehicle_type(w.type));
        return;
    }
    const VehicleType& vt = vehicle_type(w.type);
    const f32 hitch = vt.horse_drawn() ? vt.reach() + 1.8f : kHitchDist;
    // A battered cart moves slower. The laden-weight penalty (load_speed_factor) is applied ONLY to
    // a PLAYER hauling by hand - their walk speed, in the GameServer step loop - where the heaviness
    // is felt as gameplay. The hired driver/horse keeps a steady professional pace: its deterministic
    // route is what the wheel-breakdown physics (a shed wheel rolling + settling) depend on, so
    // perturbing it by the live cargo count would land the wheel in a different, sometimes unreachable
    // spot.
    // (A reinforced rig also tows faster - rig_speed_mult is 1.0 at stock level 0, so the deterministic
    // wheel/ambush tests, which run on a stock rig, are unaffected.)
    const f32 dmg = damage_speed_factor(w.health / kWagonHealth) * rig_speed_mult(rig_level_);

    // Walks a puller (horse or teamster) toward the next route waypoint, A*-routing around
    // obstacles so it never snags. It targets sparse, string-pulled nodes using the LIVE
    // position, skipping any node already reached (so a fresh path never sends it back toward
    // its own cell centre) and easing the heading so it doesn't snap or stutter per node.
    // `gate` (0..1) scales the puller's pace: it eases to 0 as the cart lags past the hitch,
    // so the puller slows/stops rather than dragging a snagged cart through a wall.
    auto walk_route = [&](Vec3& pos, f32& yaw, f32 gate) {
        const int ridx = static_cast<int>(w.progress);
        const Vec2 wp = ridx < static_cast<int>(w.route.size()) ? w.route[ridx] : w.dest;
        driver_repath_ -= dt.seconds;
        if (driver_path_.empty() || driver_path_i_ >= driver_path_.size() || driver_repath_ <= 0.0f) {
            if (collision_) {
                collision_->gather(pos, collider_scratch_);
            }
            append_walls(pos, collider_scratch_); // the teamster A* routes AROUND Mage rock walls
            // Inside a town, keep the cart OUT of the central market plaza: add the market footprint as
            // a circular keep-out so the driver rounds it on the streets instead of cutting across (the
            // open plaza has no stall colliders, so the A* would otherwise barrel straight through it).
            // Sized < kDeliverRadius so the cart can still reach the plaza edge to deliver.
            if (const auto town = worldgen::village_containing(pos.x, pos.z, wseed, kMarketKeepout)) {
                Collider mk;
                mk.shape = Collider::Shape::Cylinder;
                mk.center = Vec3{town->center.x, pos.y, town->center.y};
                mk.radius = kMarketKeepout;
                mk.y_min = pos.y - 1.0f;
                mk.y_max = pos.y + 2.0f;
                collider_scratch_.push_back(mk);
            }
            driver_path_ = astar_path({pos.x, pos.z}, wp, collider_scratch_, vt.reach() + 0.2f, pos.y);
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
            const f32 stp = std::min(d, kWagonDriverSpeed * dmg * gate * dt.seconds);
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
        pos.y = ground_at(density, pos.x, pos.z, wseed);
        // Advance to (or skip) the current route waypoint. While gated (waiting for the cart)
        // the puller isn't trying to make progress, so don't count that as being "stuck".
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
        } else if (gate < 0.25f) {
            driver_stuck_ = 0.0f; // waiting for the cart, not snagged
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

    // A hired (NPC) puller must not run off and drag a snagged cart through a wall: as the
    // cart falls past the hitch (it's stuck / being shoved out of geometry) the puller's pace
    // eases to 0, so it slows and waits for the cart instead of yanking it through.
    auto tow_gate = [&]() {
        const f32 lag = glm::length(Vec2{w.position.x - puller.x, w.position.z - puller.z}) - hitch;
        f32 gate = glm::clamp(1.0f - lag / kTowMaxSlack, 0.0f, 1.0f);
        // Anti-deadlock: if the cart stays snagged (gate pinned low - it's wedged on a building, so
        // it can't catch up and the puller keeps waiting) for a couple of seconds, the puller stops
        // waiting and pulls it free. The cart's OWN collision resolution still stops it passing
        // through walls, so it slides around the obstacle instead of the pair deadlocking in town.
        if (gate < 0.2f) {
            driver_snag_ += dt.seconds;
        } else if (gate > 0.5f) {
            driver_snag_ = 0.0f;
        }
        if (driver_snag_ > kUnstickSeconds) {
            gate = std::max(gate, 0.6f);
        }
        return gate;
    };

    const bool has_pilot = pilot_ != 0 && players_.count(pilot_) != 0u;
    const bool has_tower = tower_ != 0 && players_.count(tower_) != 0u;

    if (vt.horse_drawn() && active_mode_ == WagonMode::Driver && !has_pilot) {
        puller = horse_pos_;
        walk_route(horse_pos_, horse_yaw_, tow_gate()); // a horse pulls the carriage along the route
        puller = horse_pos_;
        speed = kWagonDriverSpeed * 1.6f * dmg;
        towed = true;
    } else if (vt.horse_drawn()) {
        // Manual carriage: the pilot drives it like a car - throttle (W/S) + rein (A/D). A
        // player can climb up and do this even in Driver mode (to drive a stuck carriage out).
        if (has_pilot) {
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
            w.position.y = ground_at(density, w.position.x, w.position.z, wseed);
        }
        // The horse rides in front along the carriage heading.
        const Vec3 hdir{std::cos(w.yaw), 0.0f, std::sin(w.yaw)};
        horse_pos_ = w.position + hdir * hitch;
        horse_pos_.y = ground_at(density, horse_pos_.x, horse_pos_.z, wseed);
        horse_yaw_ = w.yaw;
    } else if (has_tower) {
        // A player has the handles - they haul, in EITHER mode. In Driver mode this lets a
        // player grab on and pull a stuck cart free; the hired teamster (if any) stays by the
        // drawbar (not pulling) and takes over again when the player lets go.
        puller = players_.at(tower_).controller.position();
        speed = kWagonManualSpeed * 1.6f; // the tower's own walk speed already reflects size+damage
        towed = true;
        if (driver_) {
            const Vec2 bar{puller.x, puller.z};
            Vec2 dp{driver_->position.x, driver_->position.z};
            dp += (bar - dp) * std::min(1.0f, dt.seconds * 2.0f); // amble to the drawbar
            driver_->position = Vec3{dp.x, ground_at(density, dp.x, dp.y, wseed), dp.y};
            driver_->yaw = std::atan2(active_.position.z - dp.y, active_.position.x - dp.x);
        }
    } else if (active_mode_ == WagonMode::Driver && driver_) {
        puller = driver_->position;
        walk_route(driver_->position, driver_->yaw, tow_gate()); // teamster walks in front, pulling
        puller = driver_->position;
        speed = kWagonDriverSpeed * 1.6f * dmg;
        towed = true;
    }

    if (towed) {
        // A cart bogs down in water: scale the tow pace by how deep the cart sits below the
        // waterline. (A player-hauled cart is also slowed via the puller, who wades slowly too;
        // this slows the hired teamster's cart and keeps the trailer from out-running a wading
        // puller.) The lag this opens up makes the tow_gate ease the driver to match.
        const f32 cart_sub = worldgen::water_level - w.position.y;
        if (cart_sub > 0.02f) {
            speed *= glm::mix(1.0f, 0.42f, glm::smoothstep(0.0f, 1.2f, cart_sub));
        }
        // Trailer kinematics: the puller holds the drawbar at the cart's front, so the cart
        // body trails `hitch` behind and its heading SWINGS to follow the pull (capped turn
        // rate - a cart can't pivot in place or flip instantly). It eases toward its ideal
        // trailing spot rather than being snapped there, so collision can hold it back when it
        // snags (and the puller's pace gate keeps it from being dragged through walls).
        const Vec2 P{puller.x, puller.z};
        Vec2 C{w.position.x, w.position.z};
        const Vec2 toP = P - C;
        const f32 sep = glm::length(toP);
        if (sep > 0.15f) {
            const f32 want = std::atan2(toP.y, toP.x);          // face the pull
            f32 dyaw = want - w.yaw;
            while (dyaw > Pi) dyaw -= TwoPi;
            while (dyaw < -Pi) dyaw += TwoPi;
            const f32 maxStep = kCartTurnRate * dt.seconds;
            w.yaw += glm::clamp(dyaw, -maxStep, maxStep);       // capped swing, never an instant flip
        }
        const Vec2 fwd{std::cos(w.yaw), std::sin(w.yaw)};
        const Vec2 ideal = P - fwd * hitch;                     // sit hitch behind, along the body
        const Vec2 d = ideal - C;
        const f32 dl = glm::length(d);
        if (dl > 1e-4f) {
            C += (d / dl) * std::min(dl, speed * dt.seconds);
        }
        // The towed cart is a solid body too: push it out of rocks, fences, logs and town
        // walls instead of sliding through them.
        if (collision_) {
            collision_->gather(w.position, collider_scratch_);
            for (const Collider& c : collider_scratch_) {
                C = resolve_collider(c, C, vt.reach(), w.position.y, 1.5f);
            }
        }
        w.position.x = C.x;
        w.position.z = C.y;
        w.position.y = ground_at(density, w.position.x, w.position.z, wseed);
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
        // Bonus for delivering the wagon INTACT (scales with remaining health) - defending it pays.
        const f32 intact = intact_bonus_mult(w.health / kWagonHealth);
        const f32 rdist = roads::route_length(w.route);
        // RUSH bonus: a fast delivery pays extra (decays over the route's time budget).
        const f32 rush = rush_bonus_mult(contract_elapsed_, rush_expected_time(rdist));
        // FRESHNESS: perishable cargo loses value if delivered past its (tight) spoil deadline.
        const f32 fresh = perishable_value_mult(contract_elapsed_, rdist, contract_modifier(w.id));
        // STREAK: a PERFECT delivery (the whole load arrived) extends the clean-delivery streak for a
        // stacking pay bonus; losing any cargo breaks it back to nothing.
        const bool perfect = w.goods_total == 0 || cargo_.size() >= w.goods_total;
        delivery_streak_ = perfect ? std::min<u32>(delivery_streak_ + 1u, kStreakMax) : 0u;
        const f32 streak = streak_mult(delivery_streak_);
        // CONVOY: a bigger escort party earns a better contract (co-op incentive; solo = 1.0).
        const f32 convoy = convoy_mult(static_cast<u32>(players_.size()));
        // UNSCATHED: a no-casualty haul pays a premium (fades with each party member downed).
        const f32 unscathed = unscathed_mult(contract_downs_);
        // The reward (scaled by all the multipliers) PLUS a flat kill bounty for the raiders felled.
        money_ += static_cast<u32>(
                      std::lround(base * frac * intact * rush * fresh * streak * convoy * unscathed)) +
                  kill_bounty(contract_kills_);
        contract_outcome_ = 1;
        contract_phase_ = ContractPhase::Settle;
        settle_timer_ = kSettleSeconds;
        ALRYN_INFO("Wagon delivered ({}/{} crates, x{:.2f} intact, x{:.2f} convoy, x{:.2f} unscathed)! "
                   "Party money now {}",
                   cargo_.size(), w.goods_total, intact, convoy, unscathed, money_);
        end_contract_cleanup();
        return;
    }
    // Wrecked: the ambush destroyed it. A failed haul breaks the clean-delivery streak.
    if (w.health <= 0.0f) {
        contract_outcome_ = 2;
        contract_phase_ = ContractPhase::Settle;
        settle_timer_ = kSettleSeconds;
        delivery_streak_ = 0u;
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
        // A low kerb: blocks walking THROUGH the cart at ground level, but tops out well below the
        // deck so a player can jump ON TOP (the deck is a platform; foot > y_max => no sideways push).
        c.y_max = wg.position.y + 0.4f;
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
    riders_.clear();
    has_horse_ = false;
    cargo_.clear();
    goods_.clear();
    wheel_off_ = false;
    wheel_carrier_ = 0;
    wheel_repair_ = 0.0f;
    bandit_cd_ = 0.0f;
    for (auto& [pid, pl] : players_) {
        pl.carrying = false;
    }
}

// Knock a wheel off the active cart, dropping it on the ground beside the cart. The cart can't roll
// again until a player carries this wheel back to it and refits it (update_wheel).
void GameServer::force_wheel_break() {
    if (contract_phase_ != ContractPhase::Active || wheel_off_) {
        return;
    }
    wheel_off_ = true;
    wheel_carrier_ = 0;
    wheel_repair_ = 0.0f;
    const Vec3& p = active_.position;
    const VehicleType& vt = vehicle_type(active_.type);
    const std::vector<Vec3> wheels = vt.wheels();
    // Pick a RANDOM wheel to shed (not always the same corner): hash the cart id + its current
    // position (so each break, at a different spot on the road, sheds a different wheel).
    const u32 h = detail::tree_hash(static_cast<int>(active_.id),
                                    static_cast<int>(std::lround(p.x * 7.0f + p.z * 13.0f)), 0xC0FFEEu);
    wheel_index_ = static_cast<u8>(h % static_cast<u32>(std::max<usize>(1, wheels.size())));
    const Vec3 woff = wheels[wheel_index_];
    // The wheel bursts off from its own axle position on the cart (transform the local offset by the
    // cart's heading - the mesh faces local +X and is drawn rotateY(-yaw), so +X->fwd, +Z->right).
    const Vec2 fwd{std::cos(active_.yaw), std::sin(active_.yaw)};
    const Vec2 right{-std::sin(active_.yaw), std::cos(active_.yaw)};
    const f32 r = vt.wheel_radius();
    const f32 wx = p.x + fwd.x * woff.x + right.x * woff.z;
    const f32 wz = p.z + fwd.y * woff.x + right.y * woff.z;
    wheel_pos_ = Vec3{wx, worldgen::height(wx, wz, sampler_.seed()) + r, wz}; // centre rides at ground+radius
    // Launch it: rolls OFF the side it sat on, with a little of the cart's forward momentum.
    const f32 side_sign = woff.z >= 0.0f ? 1.0f : -1.0f;
    wheel_vel_ = glm::normalize(right * side_sign + fwd * 0.45f) * kWheelRollSpeed;
    bandit_cd_ = kBanditFirstDelay; // bandits close in shortly after the cart strands
    ALRYN_INFO("A wheel came off the wagon - chase it down, fetch it and refit it!");
}

void GameServer::debug_place_player(net::PlayerId id, const Vec3& pos) {
    const auto it = players_.find(id);
    if (it != players_.end()) {
        it->second.controller.set_position(pos);
    }
}

void GameServer::unlock_tier(net::PlayerId id, u8 tier) {
    const auto it = players_.find(id);
    if (it != players_.end()) {
        it->second.owned_tier =
            std::max(it->second.owned_tier, static_cast<u8>(std::min<u8>(tier, kTierCount - 1u)));
    }
}

// Wheel-breakdown event: while rolling, a wheel can work loose (the cart then halts in update_wagon);
// a player picks up the fallen wheel (E) and carries it back, and holding it by the cart channels the
// re-attach. On refit the cart rolls again and the next break is armed.
void GameServer::update_wheel(Timestep dt, const DensitySampler& density) {
    (void)density;
    if (contract_phase_ != ContractPhase::Active) {
        return;
    }
    Wagon& w = active_;
    if (!wheel_off_) {
        // Wheels only shed while the cart is actually rolling (a parked cart is fine).
        if (glm::length(wagon_vel_) > 0.4f) {
            wheel_break_cd_ -= dt.seconds;
            if (wheel_break_cd_ <= 0.0f) {
                force_wheel_break();
            }
        }
        return;
    }
    if (wheel_carrier_ == 0) {
        // Loose on the ground: roll it physically (initial burst velocity decaying under friction)
        // until it settles, then it lies waiting to be fetched. Friction-only so it always stops
        // within a bounded distance (no rolling off forever down a hillside).
        if (glm::length(wheel_vel_) > 1e-3f) {
            const u32 seed = sampler_.seed();
            const f32 r = vehicle_type(w.type).wheel_radius();
            Vec2 p2{wheel_pos_.x, wheel_pos_.z};
            p2 += wheel_vel_ * dt.seconds;
            // Bounce off walls / fences / buildings instead of rolling through them: if the step lands
            // inside a collider, push the wheel back out and reflect its velocity off the surface.
            if (collision_) {
                collision_->gather(Vec3{p2.x, wheel_pos_.y, p2.y}, collider_scratch_);
                for (const Collider& c : collider_scratch_) {
                    const Vec2 pushed = resolve_collider(c, p2, r, wheel_pos_.y - r, 2.0f * r);
                    if (glm::length(pushed - p2) > 1e-4f) {
                        const Vec2 n = glm::normalize(pushed - p2); // outward surface normal
                        wheel_vel_ = glm::reflect(wheel_vel_, n) * kWheelBounce;
                        p2 = pushed;
                        break; // one bounce per tick
                    }
                }
            }
            wheel_vel_ *= std::max(0.0f, 1.0f - kWheelRollDrag * dt.seconds); // friction
            if (glm::length(wheel_vel_) < kWheelRollStop) {
                wheel_vel_ = Vec2{0.0f}; // settled
            }
            wheel_pos_ = Vec3{p2.x, worldgen::height(p2.x, p2.y, seed) + r, p2.y};
        }
        return; // waiting to be picked up
    }
    wheel_vel_ = Vec2{0.0f}; // carried - no longer rolling
    const auto it = players_.find(wheel_carrier_);
    if (it == players_.end()) {
        wheel_carrier_ = 0; // the carrier left - the wheel drops where it was
        return;
    }
    const Vec3 cp = it->second.controller.position();
    wheel_pos_ = cp + Vec3{0.0f, 0.6f, 0.0f}; // carried at the waist
    if (glm::length(Vec2{cp.x - w.position.x, cp.z - w.position.z}) < kWheelAttachRange) {
        wheel_repair_ += dt.seconds / kWheelRepairTime; // hold by the cart to refit
        if (wheel_repair_ >= 1.0f) {
            wheel_off_ = false;
            wheel_carrier_ = 0;
            wheel_repair_ = 0.0f;
            // Re-arm the next break with a fresh randomised interval (so it isn't a fixed cadence) -
            // hashed off the cart's current position, which has moved since the last break.
            wheel_break_cd_ =
                kWheelBreakMinTime +
                (kWheelBreakAvgTime - kWheelBreakMinTime) *
                    detail::hash01(active_.id ^ 0x5733EEDu ^
                                   static_cast<u32>(std::lround(active_.position.x + active_.position.z)));
            ALRYN_INFO("Wheel refitted - the wagon rolls again.");
        }
    } else {
        wheel_repair_ = std::max(0.0f, wheel_repair_ - dt.seconds / kWheelRepairTime); // slips if you stray
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

// The active cart's bed is a moving platform: the deck-top height where (x,z) sits over the cart
// footprint (else a large-negative sentinel). The controller maxes this with the bridge height, so a
// player who jumps up gets caught on the deck (foot >= deck - step_height) and stands on top.
f32 GameServer::wagon_top_at(f32 x, f32 z) const {
    if (contract_phase_ != ContractPhase::Active) {
        return -1e9f;
    }
    const VehicleType& vt = vehicle_type(active_.type);
    const Vec2 fp = vt.footprint();
    const Vec2 rel{x - active_.position.x, z - active_.position.z};
    const Vec2 fwd{std::cos(active_.yaw), std::sin(active_.yaw)};
    const Vec2 right{-std::sin(active_.yaw), std::cos(active_.yaw)};
    const f32 lx = glm::dot(rel, fwd);
    const f32 lz = glm::dot(rel, right);
    const f32 pad = 0.15f; // a little forgiveness so you don't slip off at the very rail
    if (std::abs(lx) > fp.x + pad || std::abs(lz) > fp.y + pad) {
        return -1e9f;
    }
    return active_.position.y + vt.deck_height();
}

// After the cart has moved this tick (delta = its xz step), carry along any player standing ON TOP of
// it (within the footprint, at deck height) so they ride with the wagon. Pilot/handler/seated riders
// are positioned elsewhere, so they're skipped here.
void GameServer::carry_top_riders(const Vec2& delta, const VehicleType& vt) {
    if (glm::length(delta) < 1e-5f) {
        return;
    }
    const f32 deck = active_.position.y + vt.deck_height();
    const Vec2 fp = vt.footprint();
    const Vec2 fwd{std::cos(active_.yaw), std::sin(active_.yaw)};
    const Vec2 right{-std::sin(active_.yaw), std::cos(active_.yaw)};
    for (auto& [id, pl] : players_) {
        if (id == tower_ || id == pilot_ || riders_.count(id) != 0u) {
            continue;
        }
        const Vec3 pp = pl.controller.position();
        const Vec2 rel{pp.x - active_.position.x, pp.z - active_.position.z};
        if (std::abs(glm::dot(rel, fwd)) > fp.x + 0.25f || std::abs(glm::dot(rel, right)) > fp.y + 0.25f) {
            continue; // not over the bed
        }
        if (pp.y < deck - 0.45f || pp.y > deck + 1.2f) {
            continue; // not standing on the deck (on the ground beside it, or airborne above)
        }
        pl.controller.set_position(pp + Vec3{delta.x, 0.0f, delta.y});
    }
}

void GameServer::update_ambush(Timestep dt, const DensitySampler& density) {
    Wagon& w = active_;
    // Debug: stop the wagon ambushes entirely - clear any raiders in progress and never spawn more.
    if (debug_no_ambush_) {
        ambush_.clear();
        return;
    }
    // So ambushers chasing the cart cross bridges with it instead of dropping into the river.
    const u32 wseed = sampler_.seed();
    const auto bridge = [wseed](f32 x, f32 z) { return roads::bridge_height(x, z, wseed); };

    // Spawn ambushers in up to two waves as the wagon travels.
    auto spawn_wave = [&](u32 count) {
        for (u32 k = 0; k < count; ++k) {
            const u32 h = detail::tree_hash(static_cast<int>(w.id), static_cast<int>(next_ambush_id_),
                                            7000u);
            Enemy e;
            e.id = next_ambush_id_++;
            const u32 roll = h % 8u;
            // ~1/8 each of brute / archer / shield-bearer / healer / sapper / warlord, the rest grunts.
            e.kind = roll == 0u   ? 2u
                     : roll == 1u ? 3u
                     : roll == 2u ? kEnemyShield
                     : roll == 3u ? kEnemyHealer
                     : roll == 4u ? kEnemySapper
                     : roll == 5u ? kEnemyWarlord
                                  : 0u;
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
    const u32 total = ambush_count(w.difficulty, contract_modifier(w.id));
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

    // A stranded cart (a wheel is off, being refitted) is a sitting duck: opportunist bandits close
    // in on a timer while it's down, so the party has to defend the repair. (The cart is halted, so
    // the travel-progress waves above don't advance meanwhile - these are the only fresh spawns.)
    if (wheel_off_ && left_town) {
        bandit_cd_ -= dt.seconds;
        if (bandit_cd_ <= 0.0f && ambush_.size() < kRepairBanditCap) {
            spawn_wave(kBanditWaveSize);
            bandit_cd_ = kBanditWaveInterval;
        }
    }

    // Steer ambushers at the nearest player (else the wagon) and have them strike.
    for (Enemy& e : ambush_) {
        if (collision_) {
            collision_->gather(e.position, collider_scratch_);
        }
        append_walls(e.position, collider_scratch_); // ambushers steer around Mage rock walls too
        if (e.taunt_cd > 0.0f) {
            e.taunt_cd -= dt.seconds;
        }
        if (e.stagger > 0.0f) {
            // Reeling from a heavy hit: no movement or attack this frame (step_enemy still resolves
            // knockback + terrain + ticks the stagger timer down), opening a follow-up combo window.
            step_enemy(e, density, collider_scratch_, e.position, dt, 0.0f, bridge);
            continue;
        }
        Vec3 goal = w.position;
        f32 best = kAggroRadius;
        ServerPlayer* victim = nullptr;
        // A Knight's taunt overrides target selection: the enemy fixates on the taunter.
        if (e.taunt_cd > 0.0f) {
            const auto it = players_.find(e.taunt_by);
            if (it != players_.end()) {
                victim = &it->second;
                goal = it->second.controller.position();
                best = glm::length(goal - e.position);
            }
        }
        for (auto& [id, pl] : players_) {
            const f32 d = glm::length(pl.controller.position() - e.position);
            if (d < best) {
                best = d;
                victim = &pl;
                goal = pl.controller.position();
            }
        }

        if (e.kind == kEnemyHealer) {
            // Hang back out of reach (kite from the nearest player) and mend the most-wounded raider.
            const Vec3 target = victim != nullptr ? victim->controller.position() : w.position;
            const f32 td = glm::length(target - e.position);
            Vec3 g = goal;
            if (td < kHealerKeepDist) {
                Vec3 away = e.position - target;
                away.y = 0.0f;
                if (glm::length(away) > 1e-3f) {
                    g = e.position + glm::normalize(away) * 5.0f;
                }
            }
            step_enemy(e, density, collider_scratch_, g, dt, kEnemySpeed, bridge);
            const int widx = most_wounded_ally(e, std::span<const Enemy>(ambush_), kHealerRange);
            if (widx >= 0) {
                Enemy& ally = ambush_[static_cast<usize>(widx)];
                ally.health = std::min(enemy_max_health(ally.kind),
                                       ally.health + kHealerHealRate * dt.seconds);
            }
            continue;
        }

        if (e.kind == 3u) { // archer: kite to range, AIM (telegraph), then loose a heavy arrow
            const Vec3 target = victim != nullptr ? victim->controller.position() : w.position;
            const f32 td = glm::length(target - e.position);
            if (e.slam_windup > 0.0f) {
                // Aiming: plant + wind up; loose a heavy, fast arrow when the telegraph elapses.
                e.slam_windup -= dt.seconds;
                step_enemy(e, density, collider_scratch_, e.position, dt, 0.0f, bridge);
                if (e.slam_windup <= 0.0f) {
                    e.slam_windup = 0.0f;
                    const Vec3 from = e.position + Vec3{0.0f, 1.3f, 0.0f};
                    Vec3 dir = (target + Vec3{0.0f, 0.6f, 0.0f}) - from;
                    if (glm::length(dir) > 0.3f) {
                        Projectile arrow;
                        arrow.position = from + glm::normalize(dir) * 0.5f;
                        arrow.velocity = glm::normalize(dir) * kAimedArrowSpeed;
                        arrow.kind = 1;
                        arrow.hostile = true;
                        arrow.radius = 0.15f;
                        arrow.damage = kAimedShotDamage; // heavy (the hostile-hit site reads pr.damage)
                        arrow.life = 3.0f;
                        projectiles_.push_back(arrow);
                    }
                    e.attack_cd = kArcherInterval;
                }
            } else {
                // Kite to/around range; begin aiming once in range + off cooldown.
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
                step_enemy(e, density, collider_scratch_, g, dt, kEnemySpeed, bridge);
                if (e.attack_cd <= 0.0f && td < kArcherShootRange) {
                    e.slam_windup = kAimWindup; // start the aim telegraph
                }
            }
            continue;
        }

        if (e.kind == kEnemySapper) {
            // Sapper: ignores the players and rushes the WAGON, then DETONATES on contact - a heavy
            // hit to the cargo + a small blast on nearby players. Intercept it before it arrives!
            step_enemy(e, density, collider_scratch_, w.position, dt, kSapperSpeed, bridge);
            if (glm::length(w.position - e.position) < kSapperRange) {
                if (!debug_god_) {
                    w.health -= kSapperDamage * rig_damage_mult(rig_level_);
                }
                for (auto& [pid, pl] : players_) {
                    if (glm::length(pl.controller.position() - e.position) < kSapperBlastRadius) {
                        pl.take_damage(kSapperBlastDamage); // i-frames (a dodge roll) evade the blast
                    }
                }
                e.alive = false; // it's spent - blew itself up on the cart
            }
            continue;
        }

        if (e.kind == 2u) {
            // Brute: lumbers in, then a telegraphed radial SLAM the party can read + dodge out of.
            if (e.slam_windup > 0.0f) {
                e.slam_windup -= dt.seconds;
                step_enemy(e, density, collider_scratch_, e.position, dt, 0.0f, bridge); // plant + wind up
                if (e.slam_windup <= 0.0f) {
                    e.slam_windup = 0.0f;
                    for (auto& [pid, pl] : players_) {
                        if (brute_slam_hits(e.position, pl.controller.position())) {
                            pl.take_damage(kSlamDamage); // role armour / block / shield soak it
                        }
                    }
                    if (brute_slam_hits(e.position, w.position) && !debug_god_) {
                        w.health -= kSlamWagonDamage * rig_damage_mult(rig_level_);
                    }
                    e.attack_cd = kSlamCooldown;
                }
            } else {
                step_enemy(e, density, collider_scratch_, goal, dt, kEnemySpeed * 0.62f, bridge);
                if (e.attack_cd <= 0.0f) {
                    const f32 trig = kSlamRadius * 0.8f; // commit once a target is well inside the ring
                    const bool nearV = victim != nullptr && best < trig;
                    const bool nearW = glm::length(w.position - e.position) < trig;
                    if (nearV || nearW) {
                        e.slam_windup = kSlamWindup;
                    }
                }
            }
            continue;
        }

        // The lone last raider goes berserk - a faster, harder "finish it" climax (not the brute).
        const f32 enrage = is_enraged(ambush_.size(), e.kind) ? kEnrageMult : 1.0f;
        // A nearby WARLORD rallies the swarm: allied raiders march faster + hit harder until it falls.
        const f32 rally = near_warlord(e, std::span<const Enemy>(ambush_)) ? kWarlordBuff : 1.0f;
        const f32 spd = (e.kind == kEnemyShield ? kEnemySpeed * 0.85f // shield-bearer is weighed down
                                                : kEnemySpeed) *
                        enrage * rally;
        const f32 dmg = kEnemyAttackDamage * enrage * rally;
        step_enemy(e, density, collider_scratch_, goal, dt, spd, bridge);
        if (e.attack_cd <= 0.0f) {
            const f32 reach = kEnemyAttackRange + kEnemyRadius;
            if (victim != nullptr && best < reach) {
                if (victim->try_parry()) {
                    // PARRY: the shield turns the blow (no damage) AND staggers the attacker, opening a
                    // punish window - a skill-timed reward for raising the guard right as it strikes.
                    e.stagger = kStaggerDuration;
                    Vec3 kdir = e.position - victim->controller.position();
                    kdir.y = 0.0f;
                    if (glm::length(kdir) > 1e-3f) {
                        e.knockback = glm::normalize(kdir) * (kKnockbackMax * 0.5f);
                    }
                } else {
                    victim->take_damage(dmg); // role armour / block / Aegis shield soak it
                }
                e.attack_cd = kEnemyAttackInterval;
            } else if (glm::length(w.position - e.position) < reach + 0.7f) {
                if (!debug_god_) {
                    w.health -= kWagonDamage * rig_damage_mult(rig_level_); // armored sides shrug off some
                }
                e.attack_cd = kEnemyAttackInterval;
            }
        }
    }

    // A near-wrecked wagon rallies its defenders: LAST STAND ramps their outgoing damage as the cart
    // nears destruction (a comeback chance). Same for everyone - keyed on the shared cargo's health.
    const f32 last_stand = last_stand_mult(w.health / rig_max_health(rig_level_));

    // Thrown rocks hurt ambushers; hostile arrows hurt players. A spent projectile that has
    // landed (resting) no longer deals damage - it's just stuck in the ground.
    for (Projectile& pr : projectiles_) {
        if (pr.resting) {
            continue;
        }
        if (pr.hostile) {
            for (auto& [id, pl] : players_) {
                const Vec3 chest = pl.controller.position() + Vec3{0.0f, 0.9f, 0.0f};
                if (glm::length(chest - pr.position) < pr.radius + 0.55f) {
                    pl.take_damage(pr.damage > 0.0f ? pr.damage : kArrowDamage); // aimed shots hit heavy
                    pr.alive = false;
                }
            }
        } else {
            // A friendly projectile's damage is amplified if its owner is Empowered (co-op buff).
            const auto ownit = players_.find(pr.owner);
            const f32 boost = ownit != players_.end() ? ownit->second.outgoing_mult() : 1.0f;
            for (Enemy& e : ambush_) {
                const Vec3 chest = e.position + Vec3{0.0f, 0.9f, 0.0f};
                if (glm::length(chest - pr.position) < pr.radius + kEnemyRadius + 0.3f) {
                    f32 dmg = (pr.damage > 0.0f ? pr.damage : kThrowDamage) * boost * last_stand;
                    const f32 raw = dmg; // pre-block magnitude, for the sunder check
                    // A shield-bearer soaks most of a shot that strikes its front (a point back
                    // along the projectile's path is where it came from).
                    if (enemy_blocks_hit(e, pr.position - pr.velocity * 0.1f)) {
                        dmg *= (1.0f - kShieldReduction);
                    }
                    const f32 e_before = e.health;
                    e.health -= dmg;
                    if (ownit != players_.end()) {
                        ++ownit->second.hit_fx; // confirmed hit -> the shooter's client pops a hit marker
                    }
                    // A HEAVY shot (a charged ability) staggers a shield-bearer, dropping its guard for
                    // follow-ups - even one this hit blocked (the shield took the brunt + cracked).
                    if (e.kind == kEnemyShield && raw >= kSunderThreshold) {
                        e.sunder_cd = kSunderDuration;
                    }
                    if (raw >= kStaggerThreshold) {
                        e.stagger = kStaggerDuration; // a heavy hit reels any enemy (combo window)
                    }
                    Vec3 kdir = pr.velocity; // shove along the shot's flight direction (less if blocked)
                    kdir.y = 0.0f;
                    if (glm::length(kdir) > 1e-3f) {
                        e.knockback = glm::normalize(kdir) * std::min(dmg * kKnockbackPerDamage, kKnockbackMax);
                    }
                    // RAMPAGE: the felling shot stokes the shooter's kill momentum (crossing zero this hit).
                    if (e.health <= 0.0f && e_before > 0.0f) {
                        if (ownit != players_.end()) {
                            ownit->second.on_kill();
                        }
                        flinch_allies(e.position, std::span<Enemy>(ambush_)); // MORALE: rattle the pack
                    }
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
            // weapon hits as hard as the role, amplified while Empowered (co-op buff)
            f32 dmg = role_stats(pl.role).melee_damage * pl.outgoing_mult() * last_stand;
            const f32 raw = dmg; // pre-block magnitude, for the sunder check
            // A shield-bearer blocks most of a frontal swing - so flank it (or knock it loose).
            if (enemy_blocks_hit(*hit, pl.controller.position())) {
                dmg *= (1.0f - kShieldReduction);
            }
            const f32 hit_before = hit->health;
            hit->health -= dmg;
            ++pl.hit_fx; // confirmed melee hit -> the attacker's client pops a hit marker
            // LIFESTEAL: felling a raider in melee mends the attacker a little (sustain by fighting).
            if (hit->health <= 0.0f) {
                pl.heal(kMeleeKillHeal);
                if (hit_before > 0.0f) {
                    pl.on_kill(); // RAMPAGE: the felling blow stokes kill momentum
                    flinch_allies(hit->position, std::span<Enemy>(ambush_)); // MORALE: rattle the pack
                }
            }
            // A HEAVY swing (e.g. an Empowered Knight blow) staggers it, breaking its guard a while.
            if (hit->kind == kEnemyShield && raw >= kSunderThreshold) {
                hit->sunder_cd = kSunderDuration;
            }
            if (raw >= kStaggerThreshold) {
                hit->stagger = kStaggerDuration; // a heavy hit reels any enemy (combo window)
            }
            Vec3 kdir = hit->position - pl.controller.position(); // shove away from the attacker (less if blocked)
            kdir.y = 0.0f;
            if (glm::length(kdir) > 1e-3f) {
                hit->knockback = glm::normalize(kdir) * std::min(dmg * kKnockbackPerDamage, kKnockbackMax);
            }
            pl.melee_cd = 0.35f;
        }
    }

    // Cull the dead; each one removed here was felled by the party -> tally it for the kill bounty.
    contract_kills_ +=
        static_cast<u32>(std::erase_if(ambush_, [](const Enemy& e) { return !e.alive || e.health <= 0.0f; }));

    // Player health: regen out of combat, respawn at the town on death.
    for (auto& [id, pl] : players_) {
        pl.since_hit += dt.seconds;
        pl.decay_rampage(dt.seconds); // kill momentum fades if you stop felling raiders
        if (pl.health > 0.0f && pl.since_hit > kPlayerRegenDelay) {
            pl.health = std::min(pl.max_health, pl.health + kPlayerRegen * dt.seconds);
        }
        // A lethal blow: the once-per-haul SECOND WIND catches the first one (clinging on at low HP for
        // a clutch comeback); after that, slain players respawn at the town.
        if (pl.health <= 0.0f && !pl.try_second_wind()) {
            pl.controller.set_position(spawn_point(id));
            pl.health = pl.max_health;
            pl.since_hit = kPlayerRegenDelay;
            ++contract_downs_; // a real casualty this haul - costs the UNSCATHED delivery bonus
            ALRYN_INFO("Player {} was slain and respawned", id);
        }
    }

    // FIELD REPAIR: between waves (no raiders alive), a player staying near the cart patches it up,
    // mending its health slowly - so clearing a wave then tending the cart lets a battered haul recover.
    if (w.health > 0.0f && ambush_.empty()) {
        bool tended = false;
        for (const auto& [pid, pl] : players_) {
            if (glm::length(pl.controller.position() - w.position) < kFieldRepairRange) {
                tended = true;
                break;
            }
        }
        w.health = field_repair(w.health, rig_max_health(rig_level_), tended, dt.seconds);
    }
}

} // namespace alryn
