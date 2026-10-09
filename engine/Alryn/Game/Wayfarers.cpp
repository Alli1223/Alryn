// Life on the roads (Game/Wayfarer.h): the WAYFARERS who walk between the towns - lone travellers,
// pilgrims, bands of adventurers and merchant caravans - and the roadside ERRANDS a traveller in trouble
// asks of the party. Implemented as GameServer methods (like SideQuests.cpp) so an errand's foes live in
// the same hostile list the combat pass already fights and its pay lands in the party's wallet.
//
// The traffic is a light illusion kept round the party: groups are spawned out of sight (well along the
// road ahead or behind, never in view) on a real town-to-town road that runs near a hero, walk it at a
// steady pace keeping to their right, step round a hero or the cart in their way, hurry off when raiders
// show up, and vanish once nobody is near or they reach their town.
#include <Alryn/Net/GameServer.h>

#include <Alryn/Core/Density.h>
#include <Alryn/Core/Log.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/ScatterHash.h>
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/World/VehicleTypes.h>
#include <Alryn/World/Village.h>

#include <algorithm>
#include <cmath>

namespace alryn {

namespace {
// The ground under (x,z) near a known height `y_hint` (a short probe down from just above it - the road
// is walked every tick), honouring digs, craters + bridge decks.
f32 ground_near(const DensitySampler& density, f32 x, f32 z, f32 y_hint, u32 seed) {
    const f32 deck = roads::bridge_height(x, z, seed);
    if (const auto g = raycast_density(density, Vec3{x, y_hint + 3.0f, z}, Vec3{0.0f, -1.0f, 0.0f}, 9.0f)) {
        return std::max(g->y, deck > g->y - 0.5f ? deck : -1e9f);
    }
    return std::max(worldgen::height(x, z, seed), deck);
}

// Distance from p to the nearest hero.
template <typename Players>
f32 nearest_hero(const Players& players, const Vec3& p) {
    f32 best = 1e30f;
    for (const auto& [id, pl] : players) {
        const Vec3 q = pl.controller.position();
        best = std::min(best, glm::length(Vec2{q.x - p.x, q.z - p.z}));
    }
    return best;
}

CharacterAppearance wayfarer_look(u32 id) {
    CharacterAppearance a = villager_look(id ^ 0x5EED5EEDu);
    a.race = static_cast<Race>((id * 2654435761u >> 28) % kRaceCount);
    return a;
}
} // namespace

// ---- Roads as walkable routes ---------------------------------------------------------------------

Vec2 GameServer::RoadRoute::at(f32 s) const {
    if (pts.size() < 2) {
        return pts.empty() ? Vec2{0.0f} : pts.front();
    }
    s = glm::clamp(s, 0.0f, length);
    const auto it = std::upper_bound(cum.begin(), cum.end(), s);
    const usize i = std::min<usize>(static_cast<usize>(std::max<std::ptrdiff_t>(it - cum.begin(), 1)), pts.size() - 1);
    const f32 seg = cum[i] - cum[i - 1];
    const f32 t = seg > 1e-4f ? (s - cum[i - 1]) / seg : 0.0f;
    return glm::mix(pts[i - 1], pts[i], t);
}

Vec2 GameServer::RoadRoute::heading(f32 s) const {
    // Look a few metres either side, so the walkers turn smoothly through the polyline's corners.
    const Vec2 d = at(s + 3.0f) - at(s - 3.0f);
    const f32 l = glm::length(d);
    return l > 1e-4f ? d / l : Vec2{1.0f, 0.0f};
}

f32 GameServer::RoadRoute::project(const Vec2& p) const {
    f32 best = 1e30f;
    f32 best_s = 0.0f;
    for (usize i = 1; i < pts.size(); ++i) {
        const Vec2 ab = pts[i] - pts[i - 1];
        const f32 len2 = glm::dot(ab, ab);
        const f32 t = len2 > 1e-6f ? glm::clamp(glm::dot(p - pts[i - 1], ab) / len2, 0.0f, 1.0f) : 0.0f;
        const f32 d = glm::length(p - (pts[i - 1] + ab * t));
        if (d < best) {
            best = d;
            best_s = cum[i - 1] + (cum[i] - cum[i - 1]) * t;
        }
    }
    return best_s;
}

std::shared_ptr<const GameServer::RoadRoute> GameServer::road_route(const Vec2& a, const Vec2& b) {
    auto q = [](f32 v) { return static_cast<u64>(static_cast<u32>(static_cast<i32>(std::lround(v)))); };
    const u64 key = (q(a.x) * 73856093ull) ^ (q(a.y) * 19349663ull) ^ (q(b.x) * 83492791ull) ^ (q(b.y) * 2654435761ull);
    if (const auto it = route_cache_.find(key); it != route_cache_.end()) {
        return it->second;
    }
    auto route = std::make_shared<RoadRoute>();
    route->pts = roads::route_polyline(a, b, sampler_.seed());
    route->cum.resize(route->pts.size(), 0.0f);
    for (usize i = 1; i < route->pts.size(); ++i) {
        route->cum[i] = route->cum[i - 1] + glm::length(route->pts[i] - route->pts[i - 1]);
    }
    route->length = route->cum.empty() ? 0.0f : route->cum.back();
    // Where the road comes in through each end town's gate: travellers stop a few metres inside it
    // (rather than walking on into the market stalls) and go their way once nobody's watching.
    const u32 seed = sampler_.seed();
    route->stop_lo = 0.0f;
    route->stop_hi = route->length;
    for (f32 s = 2.0f; s < route->length * 0.5f; s += 2.0f) {
        const Vec2 p = route->at(s);
        if (!worldgen::inside_village(p.x, p.y, seed, -6.0f)) {
            route->stop_lo = std::max(0.0f, s - 2.0f);
            break;
        }
    }
    for (f32 s = route->length - 2.0f; s > route->length * 0.5f; s -= 2.0f) {
        const Vec2 p = route->at(s);
        if (!worldgen::inside_village(p.x, p.y, seed, -6.0f)) {
            route->stop_hi = std::min(route->length, s + 2.0f);
            break;
        }
    }
    if (route_cache_.size() > 256) {
        route_cache_.clear(); // a long journey's worth of roads; start afresh
    }
    return route_cache_.emplace(key, std::move(route)).first->second;
}

f32 GameServer::way_rand() {
    way_rng_ ^= way_rng_ << 13;
    way_rng_ ^= way_rng_ >> 17;
    way_rng_ ^= way_rng_ << 5;
    return static_cast<f32>(way_rng_ & 0xFFFFFFu) / static_cast<f32>(0xFFFFFFu);
}

// ---- Wayfarers ------------------------------------------------------------------------------------

// Sets a group out on a road near `near`: a road between two towns that passes close by, a start well along
// it out of sight (ahead or behind, `fixed_ahead` > 0 pins it that far for tests + shots), heading mostly
// toward the party (so it's met on the way). A lone traveller or pilgrim, a band of adventurers or
// pilgrims, or a merchant's caravan.
bool GameServer::spawn_way_group(const Vec3& near, bool force_caravan, f32 fixed_ahead) {
    const u32 seed = sampler_.seed();
    const Vec2 p{near.x, near.z};
    // The towns round about, and the roads from the nearest to its neighbours on the road graph.
    const int vcx = static_cast<int>(std::floor(p.x / worldgen::village_cell));
    const int vcz = static_cast<int>(std::floor(p.y / worldgen::village_cell));
    std::optional<worldgen::Village> home;
    f32 home_d = 1e30f;
    for (int dz = -2; dz <= 2; ++dz) {
        for (int dx = -2; dx <= 2; ++dx) {
            if (const auto v = worldgen::village_at(vcx + dx, vcz + dz, seed)) {
                const f32 d = glm::length(v->center - p);
                if (d < home_d) {
                    home_d = d;
                    home = v;
                }
            }
        }
    }
    if (!home) {
        return false;
    }
    std::vector<std::shared_ptr<const RoadRoute>> near_roads;
    for (const worldgen::Village& to : roads::reachable_towns(home->center, seed, 5)) {
        auto r = road_route(home->center, to.center);
        if (r->pts.size() < 2) {
            continue;
        }
        if (glm::length(r->at(r->project(p)) - p) < 45.0f) {
            near_roads.push_back(std::move(r));
        }
    }
    if (near_roads.empty()) {
        return false;
    }
    const auto route = near_roads[static_cast<usize>(way_rand() * static_cast<f32>(near_roads.size())) % near_roads.size()];
    const f32 s0 = route->project(p);
    // Out of sight along the road: ahead or behind, far enough that nobody sees them appear.
    f32 s = -1.0f;
    for (int attempt = 0; attempt < 4 && s < 0.0f; ++attempt) {
        const f32 side = way_rand() < 0.5f ? 1.0f : -1.0f;
        const f32 off = fixed_ahead > 0.0f ? fixed_ahead : glm::mix(kWayfarerSpawnMin, kWayfarerSpawnMax, way_rand());
        const f32 cand = s0 + side * off;
        if (cand < route->stop_lo + 4.0f || cand > route->stop_hi - 4.0f) {
            continue;
        }
        const Vec2 at = route->at(cand);
        if (fixed_ahead <= 0.0f && nearest_hero(players_, Vec3{at.x, 0.0f, at.y}) < kWayfarerClear) {
            continue;
        }
        if (worldgen::inside_village(at.x, at.y, seed, 4.0f)) {
            continue; // not popping up in the middle of a town
        }
        s = cand;
    }
    if (s < 0.0f) {
        return false;
    }
    WayGroup g;
    g.id = next_wayfarer_id_++;
    g.route = route;
    g.along = s;
    // Mostly walking toward the party (met on the road), sometimes the same way (overtaken / overtaking).
    const f32 toward = s0 > s ? 1.0f : -1.0f;
    g.dir = (fixed_ahead > 0.0f || way_rand() < 0.65f) ? toward : -toward; // (a scripted group comes our way)
    const f32 roll = way_rand();
    const bool caravan = force_caravan || (roll < 0.32f && std::count_if(way_groups_.begin(), way_groups_.end(),
                                                                         [](const WayGroup& o) { return o.caravan; }) <
                                                                static_cast<long>(kMaxCaravans));
    std::vector<std::pair<WayfarerRole, f32>> members; // role + slot (metres behind the lead point)
    if (caravan) {
        g.caravan = true;
        g.speed = kCaravanSpeed;
        g.cart_type = way_rand() < 0.6f ? 0u : 1u; // a handcart or a wagon
        g.beast = way_rand() < 0.55f ? 0u : 1u;
        g.load = static_cast<u8>(way_rand() * 2.99f);
        members.push_back({WayfarerRole::Merchant, -2.3f}); // at the beast's head
        const int guards = static_cast<int>(way_rand() * 2.99f);
        for (int i = 0; i < guards; ++i) {
            members.push_back({WayfarerRole::Guard, 3.4f + 1.4f * static_cast<f32>(i)});
        }
    } else if (roll < 0.6f) {
        // A band on the road: adventurers, or pilgrims walking together.
        const bool pilgrims = way_rand() < 0.4f;
        const int n = 2 + static_cast<int>(way_rand() * 2.99f);
        for (int i = 0; i < n; ++i) {
            members.push_back({pilgrims ? WayfarerRole::Pilgrim : WayfarerRole::Adventurer, 1.5f * static_cast<f32>(i)});
        }
        g.speed = kWayfarerSpeed * (pilgrims ? 0.85f : 1.05f);
    } else {
        members.push_back({way_rand() < 0.3f ? WayfarerRole::Pilgrim : WayfarerRole::Traveller, 0.0f});
        g.speed = kWayfarerSpeed * glm::mix(0.85f, 1.15f, way_rand());
    }
    if (wayfarers_.size() + members.size() > kMaxWayfarers) {
        return false;
    }
    for (usize i = 0; i < members.size(); ++i) {
        Wayfarer w;
        w.body.id = 0xF0000000u | (next_wayfarer_id_++ & 0x0FFFFFFFu);
        w.body.kind = 5;
        w.body.appearance = wayfarer_look(w.body.id);
        w.route = route;
        w.group = g.id;
        w.role = members[i].first;
        w.slot = members[i].second;
        // Bands walk two abreast; a caravan's merchant walks at the beast's flank.
        w.lateral = kWayfarerKeepRight + (i % 2 == 1 ? 0.9f : 0.0f) + (w.role == WayfarerRole::Merchant ? 0.7f : 0.0f);
        const Vec2 at = route->at(s - g.dir * w.slot);
        w.body.position = Vec3{at.x, worldgen::height(at.x, at.y, seed), at.y};
        wayfarers_.push_back(w);
    }
    if (g.caravan) {
        const Vec2 at = route->at(s);
        g.cart_pos = Vec3{at.x, worldgen::height(at.x, at.y, seed), at.y};
        g.beast_pos = g.cart_pos;
    }
    way_groups_.push_back(g);
    return true;
}

bool GameServer::debug_spawn_wayfarers(const Vec3& near, bool caravan, f32 ahead) {
    return spawn_way_group(near, caravan, ahead);
}

void GameServer::update_wayfarers(Timestep dt, const DensitySampler& density) {
    if (players_.empty()) {
        wayfarers_.clear();
        way_groups_.clear();
        return;
    }
    const u32 seed = sampler_.seed();
    // Spawning: keep a few groups on the road round each hero who's out on (or near) one.
    wayfarer_scan_cd_ -= dt.seconds;
    if (road_life_ && wayfarer_scan_cd_ <= 0.0f) {
        wayfarer_scan_cd_ = kWayfarerScan;
        for (const auto& [pid, pl] : players_) {
            const Vec3 p = pl.controller.position();
            if (roads::distance(p.x, p.z, seed) > 50.0f) {
                continue;
            }
            int near = 0;
            for (const WayGroup& g : way_groups_) {
                if (glm::length(g.route->at(g.along) - Vec2{p.x, p.z}) < kWayfarerDespawn * 0.85f) {
                    ++near;
                }
            }
            if (near < kWayfarerGroupsNear && wayfarers_.size() < kMaxWayfarers) {
                spawn_way_group(p, false, 0.0f);
                break; // one new group per look
            }
        }
    }

    // Walking: each group's lead point advances along its road; its members keep their slots, to their
    // right of the centreline, stepping aside for a hero or the cart in the way.
    const Wagon* cart = contract_phase_ == ContractPhase::Active ? &active_ : nullptr;
    for (WayGroup& g : way_groups_) {
        // Raiders about: hurry on (the way they're already going - away from trouble ahead turns them round).
        g.hurry = std::max(0.0f, g.hurry - dt.seconds);
        const Vec2 lead = g.route->at(g.along);
        for (const Enemy& e : ambush_) {
            const Vec2 to{e.position.x - lead.x, e.position.z - lead.y};
            if (glm::length(to) < 16.0f) {
                if (g.hurry <= 0.0f && glm::dot(to, g.route->heading(g.along) * g.dir) > 0.0f) {
                    g.dir = -g.dir; // trouble ahead: turn back
                }
                g.hurry = 6.0f;
            }
        }
        const f32 pace = g.speed * (g.hurry > 0.0f ? kWayfarerHurry : 1.0f);
        if (!g.arrived) {
            g.along += pace * g.dir * dt.seconds;
            if (g.along <= g.route->stop_lo || g.along >= g.route->stop_hi) {
                g.along = glm::clamp(g.along, g.route->stop_lo, g.route->stop_hi);
                g.arrived = true; // in through the gate - they'll be gone once nobody's watching
            }
        }
        if (g.caravan) {
            const Vec2 fwd = g.route->heading(g.along) * g.dir;
            const Vec2 right{-fwd.y, fwd.x};
            const Vec2 c = g.route->at(g.along) + right * kWayfarerKeepRight;
            const Vec2 b = g.route->at(g.along + g.dir * 2.9f) + right * kWayfarerKeepRight;
            g.cart_pos = Vec3{c.x, ground_near(density, c.x, c.y, g.cart_pos.y, seed), c.y};
            g.cart_yaw = std::atan2(fwd.y, fwd.x);
            const Vec2 bf = glm::length(b - c) > 1e-3f ? glm::normalize(b - c) : fwd;
            g.beast_pos = Vec3{b.x, ground_near(density, b.x, b.y, g.beast_pos.y, seed), b.y};
            g.beast_yaw = std::atan2(bf.y, bf.x);
        }
    }
    for (Wayfarer& w : wayfarers_) {
        const auto git = std::find_if(way_groups_.begin(), way_groups_.end(), [&](const WayGroup& g) { return g.id == w.group; });
        if (git == way_groups_.end()) {
            continue;
        }
        const WayGroup& g = *git;
        const f32 s = g.along - g.dir * w.slot;
        const Vec2 fwd = g.route->heading(s) * g.dir;
        const Vec2 right{-fwd.y, fwd.x};
        Vec2 target = g.route->at(s) + right * (w.lateral + w.dodge);
        // Step aside (further right) for a hero or the escorted cart standing in the way.
        f32 want_dodge = 0.0f;
        for (const auto& [pid, pl] : players_) {
            const Vec3 q = pl.controller.position();
            if (glm::length(Vec2{q.x, q.z} - target) < 1.6f) {
                want_dodge = 1.8f;
            }
        }
        if (cart != nullptr && glm::length(Vec2{cart->position.x, cart->position.z} - target) < 3.0f) {
            want_dodge = 3.2f;
        }
        w.dodge += (want_dodge - w.dodge) * std::min(1.0f, dt.seconds * 3.0f);
        target = g.route->at(s) + right * (w.lateral + w.dodge);
        const Vec2 prev{w.body.position.x, w.body.position.z};
        const f32 moved = glm::length(target - prev);
        w.body.position = Vec3{target.x, ground_near(density, target.x, target.y, w.body.position.y, seed), target.y};
        w.body.speed = dt.seconds > 0.0f ? std::min(moved / dt.seconds, 6.0f) : 0.0f;
        if (moved > 1e-3f) {
            w.body.yaw = std::atan2(target.y - prev.y, target.x - prev.x);
        }
    }

    // Gone once nobody's near (or they reached their town unseen).
    std::vector<u32> gone;
    for (const WayGroup& g : way_groups_) {
        const Vec2 at = g.route->at(g.along);
        const f32 d = nearest_hero(players_, Vec3{at.x, 0.0f, at.y});
        if (d > kWayfarerDespawn || (g.arrived && d > 45.0f)) {
            gone.push_back(g.id);
        }
    }
    for (const u32 id : gone) {
        std::erase_if(wayfarers_, [id](const Wayfarer& w) { return w.group == id; });
        std::erase_if(way_groups_, [id](const WayGroup& g) { return g.id == id; });
    }
}

// ---- Errands --------------------------------------------------------------------------------------

bool GameServer::errand_owns(u32 id) const {
    return std::any_of(errands_.begin(), errands_.end(), [id](const Errand& e) { return e.id == id; });
}

// A traveller in trouble waits by the road a little way ahead of `near` (along `toward`, the way the
// party's heading, if known), on the verge facing the road: the errand is offered.
bool GameServer::spawn_errand(ErrandKind kind, const Vec3& near, const Vec2& toward) {
    const u32 seed = sampler_.seed();
    const auto snap = roads::nearest_point(near.x, near.z, seed);
    if (!snap || glm::length(*snap - Vec2{near.x, near.z}) > 30.0f) {
        return false;
    }
    // Walk the road ahead in 4 m steps (re-snapping to it as it bends), the way the party is going.
    Vec2 q = *snap;
    Vec2 dir = roads::tangent(q.x, q.y, seed);
    if (glm::length(toward) > 1e-3f && glm::dot(dir, toward) < 0.0f) {
        dir = -dir;
    }
    const f32 ahead = glm::mix(kErrandAheadMin, kErrandAheadMax, way_rand());
    // On the verge to one side (clear of the road, towns, water + bridges), from `ahead` metres up the road
    // - or a little further on, if that stretch won't do.
    std::optional<Vec2> spot;
    Vec2 side{0.0f};
    const f32 sg = way_rand() < 0.5f ? 1.0f : -1.0f;
    for (f32 walked = 0.0f; walked < ahead + 64.0f && !spot; walked += 4.0f) {
        if (walked >= ahead) {
            side = Vec2{-dir.y, dir.x};
            for (const f32 sgn : {sg, -sg}) {
                const Vec2 c = q + side * sgn * (roads::road_half_width + (kind == ErrandKind::StuckCart ? 3.8f : 1.8f));
                if (worldgen::height(c.x, c.y, seed) > worldgen::water_level + 0.8f &&
                    !worldgen::inside_village(c.x, c.y, seed, 12.0f) && roads::bridge_height(c.x, c.y, seed) < -1e8f &&
                    roads::distance(c.x, c.y, seed) > roads::road_half_width + 0.8f) {
                    spot = c;
                    break;
                }
            }
            if (spot) {
                break;
            }
        }
        const Vec2 next = q + dir * 4.0f;
        const auto s = roads::nearest_point(next.x, next.y, seed);
        if (!s) {
            break;
        }
        Vec2 t = roads::tangent(s->x, s->y, seed);
        if (glm::dot(t, dir) < 0.0f) {
            t = -t;
        }
        q = *s;
        dir = t;
    }
    if (!spot) {
        return false;
    }
    const DensitySampler density = sampler_.as_sampler();
    Errand e;
    e.id = 0xE0000000u | (next_errand_id_++ & 0x0FFFFFFFu);
    e.kind = kind;
    e.phase = QuestPhase::Offered;
    const u8 max_danger = progression_ ? std::clamp<u8>(max_danger_for_level(party_level()), 1, 3) : 3;
    e.danger = static_cast<u8>(1u + static_cast<u32>(way_rand() * 2.99f) % max_danger);
    e.goal = errand_goal(kind, e.danger);
    e.reward = errand_reward(kind, e.danger);
    e.xp = errand_xp(kind, e.danger);
    e.road_dir = dir;
    e.giver.id = 0xD0000000u | (e.id & 0x0FFFFFFFu);
    e.giver.kind = 6;
    e.giver.appearance = villager_look(e.giver.id);
    const Vec2 face = glm::normalize(q - *spot); // toward the road
    e.giver.yaw = std::atan2(face.y, face.x);
    e.giver.position = Vec3{spot->x, ground_near(density, spot->x, spot->y, worldgen::height(spot->x, spot->y, seed), seed), spot->y};
    // Where the help's needed: out in the wilds on the giver's side of the road (the goat ran, the
    // brigands camp, the wolves came from), back along the road (the satchels), or right here (the cart).
    const Vec2 out = glm::normalize(*spot - q);
    switch (kind) {
        case ErrandKind::LostGoat:
        case ErrandKind::Brigands:
        case ErrandKind::Wolves: {
            const f32 reach = kind == ErrandKind::LostGoat ? glm::mix(18.0f, 28.0f, way_rand()) : glm::mix(24.0f, 32.0f, way_rand());
            Vec2 best = *spot + out * reach;
            for (int k = 0; k < 8; ++k) { // swing round off the giver's side until it's dry, walkable land
                const f32 a = (static_cast<f32>(k % 2 == 0 ? k / 2 : -(k + 1) / 2)) * 0.35f;
                const Vec2 d{out.x * std::cos(a) - out.y * std::sin(a), out.x * std::sin(a) + out.y * std::cos(a)};
                const Vec2 c = *spot + d * reach;
                if (worldgen::height(c.x, c.y, seed) > worldgen::water_level + 1.0f && roads::distance(c.x, c.y, seed) > 9.0f &&
                    !worldgen::inside_village(c.x, c.y, seed, 10.0f)) {
                    best = c;
                    break;
                }
            }
            e.site = Vec3{best.x, worldgen::height(best.x, best.y, seed), best.y};
            break;
        }
        case ErrandKind::LostParcels:
            e.site = Vec3{q.x, worldgen::height(q.x, q.y, seed), q.y} - Vec3{dir.x, 0.0f, dir.y} * 22.0f;
            break;
        case ErrandKind::StuckCart:
            e.site = e.giver.position;
            break;
    }
    e.giver.target = e.giver.position;
    // The stuck cart sinks into the verge beside its owner from the start (it's why they're stopped).
    if (kind == ErrandKind::StuckCart) {
        const Vec2 cp = *spot - dir * 2.6f;
        QuestItem it;
        it.id = next_quest_item_++;
        it.quest = e.id;
        it.position = Vec3{cp.x, ground_near(density, cp.x, cp.y, e.giver.position.y, seed), cp.y};
        it.kind = 4;
        it.state = 0;
        it.yaw = std::atan2(dir.y, dir.x);
        quest_items_.push_back(it);
        e.site = it.position;
    }
    errands_.push_back(e);
    last_errand_kind_ = static_cast<u8>(kind);
    ALRYN_INFO("A traveller by the road: '{}'", errand_title(kind));
    return true;
}

bool GameServer::debug_spawn_errand(ErrandKind kind, const Vec3& near, const Vec2& toward) {
    for (const Errand& e : errands_) {
        const u32 gone = e.id;
        std::erase_if(ambush_, [gone](const Enemy& en) { return en.quest == gone; });
        std::erase_if(quest_items_, [gone](const QuestItem& it) { return it.quest == gone; });
    }
    errands_.clear();
    return spawn_errand(kind, near, toward);
}

// The errand's taken: put out what it's about.
void GameServer::wake_errand(Errand& e, const DensitySampler& density) {
    const u32 seed = sampler_.seed();
    auto spawn_foe = [&](u8 kind, const Vec3& home, f32 ang, f32 rad) {
        Enemy en;
        en.id = next_ambush_id_++;
        en.kind = kind;
        en.health = enemy_max_health(kind);
        en.quest = e.id;
        const f32 x = e.site.x + std::cos(ang) * rad, z = e.site.z + std::sin(ang) * rad;
        en.position = Vec3{x, ground_near(density, x, z, worldgen::height(x, z, seed), seed), z};
        en.home = home;
        en.yaw = ang + Pi;
        ambush_.push_back(en);
    };
    switch (e.kind) {
        case ErrandKind::LostGoat: {
            QuestItem it;
            it.id = next_quest_item_++;
            it.quest = e.id;
            it.position = e.site;
            it.kind = 2;
            it.yaw = way_rand() * TwoPi;
            quest_items_.push_back(it);
            break;
        }
        case ErrandKind::Brigands: {
            // A cutthroat or two, an archer once it's dangerous - camped round the site, guarding it.
            static constexpr u8 kCrew[] = {0u, 0u, 3u, 0u};
            for (u8 i = 0; i < e.goal; ++i) {
                spawn_foe(kCrew[i % 4u], e.site, TwoPi * static_cast<f32>(i) / static_cast<f32>(e.goal), 2.5f);
            }
            break;
        }
        case ErrandKind::Wolves: {
            // The pack comes out of the trees for the traveller (their home is the road: they come to it).
            for (u8 i = 0; i < e.goal; ++i) {
                spawn_foe(kEnemyWolf, e.giver.position, TwoPi * static_cast<f32>(i) / static_cast<f32>(e.goal), 3.0f);
            }
            break;
        }
        case ErrandKind::LostParcels: {
            // Spilled along the road behind (toward where the party came from), in the verge or the grass.
            for (u8 i = 0; i < e.goal; ++i) {
                const f32 back = 10.0f + 13.0f * static_cast<f32>(i) + 5.0f * way_rand();
                Vec2 c = Vec2{e.giver.position.x, e.giver.position.z} - e.road_dir * back;
                if (const auto s = roads::nearest_point(c.x, c.y, seed)) {
                    const Vec2 t = roads::tangent(s->x, s->y, seed);
                    const f32 sg = way_rand() < 0.5f ? 1.0f : -1.0f;
                    c = *s + Vec2{-t.y, t.x} * sg * (roads::road_half_width + 0.8f + 4.0f * way_rand());
                }
                QuestItem it;
                it.id = next_quest_item_++;
                it.quest = e.id;
                it.position = Vec3{c.x, ground_near(density, c.x, c.y, worldgen::height(c.x, c.y, seed), seed), c.y};
                it.kind = 3;
                it.yaw = way_rand() * TwoPi;
                quest_items_.push_back(it);
            }
            break;
        }
        case ErrandKind::StuckCart:
            break; // the cart's been sitting there all along
    }
}

void GameServer::finish_errand(Errand& e) {
    e.phase = QuestPhase::Complete;
    e.progress = e.goal;
    e.banner = kErrandThanksSeconds;
    money_ += e.reward;
    for (auto& [id, pl] : players_) {
        award_xp(pl, e.xp);
    }
    ALRYN_INFO("Errand done: {} (+{} gold, +{} xp each)", errand_title(e.kind), e.reward, e.xp);
}

bool GameServer::errand_foe_felled(u32 id) {
    for (Errand& e : errands_) {
        if (e.id != id) {
            continue;
        }
        if (e.phase == QuestPhase::Active) {
            e.progress = static_cast<u8>(std::min<u32>(e.progress + 1u, e.goal));
            if (e.progress >= e.goal) {
                finish_errand(e);
            }
        }
        return true;
    }
    return false;
}

void GameServer::errand_dig(const Vec3& at) {
    for (Errand& e : errands_) {
        if (e.kind != ErrandKind::StuckCart || e.phase != QuestPhase::Active) {
            continue;
        }
        for (QuestItem& it : quest_items_) {
            if (it.quest != e.id || it.kind != 4u || it.state != 0u ||
                glm::length(Vec2{at.x - it.position.x, at.z - it.position.z}) > kStuckDigReach) {
                continue;
            }
            e.progress = static_cast<u8>(std::min<u32>(e.progress + 1u, e.goal));
            if (e.progress >= e.goal) {
                it.state = 1u; // the wheel rolls up out of the mud
                finish_errand(e);
            }
        }
    }
}

void GameServer::update_errands(Timestep dt, const DensitySampler& density) {
    if (players_.empty()) {
        return;
    }
    const u32 seed = sampler_.seed();
    Vec3 centroid{0.0f};
    for (const auto& [id, pl] : players_) {
        centroid += pl.controller.position();
    }
    centroid /= static_cast<f32>(players_.size());
    // Which way the party's travelling (smoothed), for putting the next traveller ahead of it.
    const Vec2 moved{centroid.x - errand_probe_.x, centroid.z - errand_probe_.z};
    if (glm::length(moved) > 0.05f && glm::length(moved) < 5.0f) {
        party_heading_ = glm::mix(party_heading_, glm::normalize(moved), std::min(1.0f, dt.seconds * 1.5f));
    }
    errand_probe_ = centroid;

    // The director: out on the open road (not in or by a town, on or near a road), after a while of travel,
    // a traveller in trouble turns up ahead - one at a time, a different kind from the last.
    const bool travelling = !worldgen::inside_village(centroid.x, centroid.z, seed, 18.0f) &&
                            roads::distance(centroid.x, centroid.z, seed) < 14.0f;
    if (road_life_ && errands_.empty() && travelling) {
        errand_cd_ -= dt.seconds;
        if (errand_cd_ <= 0.0f) {
            u8 k = static_cast<u8>(way_rand() * static_cast<f32>(kErrandKinds)) % kErrandKinds;
            if (k == last_errand_kind_) {
                k = static_cast<u8>((k + 1u) % kErrandKinds);
            }
            if (spawn_errand(static_cast<ErrandKind>(k), centroid, party_heading_)) {
                errand_cd_ = glm::mix(kErrandCooldownMin, kErrandCooldownMax, way_rand());
            } else {
                errand_cd_ = 4.0f; // nowhere fit just here - look again a little further on
            }
        }
    }

    for (Errand& e : errands_) {
        const f32 near_giver = nearest_hero(players_, e.giver.position);
        switch (e.phase) {
            case QuestPhase::Offered:
                // Talked to (the client sends the errand's id as its pick, standing by the traveller).
                for (const auto& [id, pl] : players_) {
                    if (pl.input.quest_pick == e.id &&
                        glm::length(pl.controller.position() - e.giver.position) < kErrandTalkRange + 2.5f) {
                        e.phase = QuestPhase::Active;
                        wake_errand(e, density);
                        ALRYN_INFO("Errand taken: {}", errand_title(e.kind));
                        break;
                    }
                }
                break;
            case QuestPhase::Active: {
                // The goat: E by her and she trots after that hero; brought to her owner, it's done.
                // A satchel: E by it to pick it up.
                for (auto& [id, pl] : players_) {
                    if (!pl.input.grab) {
                        continue;
                    }
                    const Vec3 p = pl.controller.position();
                    for (QuestItem& it : quest_items_) {
                        if (it.quest != e.id) {
                            continue;
                        }
                        const f32 d = glm::length(Vec2{p.x - it.position.x, p.z - it.position.z});
                        if (it.kind == 2u && it.state == 0u && d < kGoatPickRange) {
                            it.state = 1u;
                            e.leader = id;
                        } else if (it.kind == 3u && it.state == 0u && d < kParcelPickRange) {
                            it.state = 1u;
                            e.progress = static_cast<u8>(std::min<u32>(e.progress + 1u, e.goal));
                            if (e.progress >= e.goal) {
                                finish_errand(e);
                            }
                        }
                    }
                }
                std::erase_if(quest_items_, [&](const QuestItem& it) { return it.quest == e.id && it.kind == 3u && it.state != 0u; });
                for (QuestItem& it : quest_items_) {
                    if (it.quest != e.id || it.kind != 2u) {
                        continue;
                    }
                    if (it.state == 1u) {
                        const auto lp = players_.find(e.leader);
                        if (lp == players_.end()) {
                            it.state = 0u; // her friend's gone - she stops where she is
                            continue;
                        }
                        const Vec3 p = lp->second.controller.position();
                        Vec2 to{p.x - it.position.x, p.z - it.position.z};
                        const f32 d = glm::length(to);
                        if (d > kGoatFollow) {
                            const f32 step = std::min(d - kGoatFollow, std::min(6.5f, d * 1.6f) * dt.seconds);
                            to /= d;
                            it.position.x += to.x * step;
                            it.position.z += to.y * step;
                            it.yaw = std::atan2(to.y, to.x);
                        }
                        it.position.y = ground_near(density, it.position.x, it.position.z, it.position.y, seed);
                        if (glm::length(Vec2{it.position.x - e.giver.position.x, it.position.z - e.giver.position.z}) <
                            kGoatHomeRange) {
                            it.state = 2u; // home with her owner
                            finish_errand(e);
                        }
                    } else if (it.state == 0u) {
                        // Loose: she ambles round where she ran to, cropping the grass.
                        const f32 t = static_cast<f32>(tick_) / 60.0f;
                        const f32 a = t * 0.35f + static_cast<f32>(it.id);
                        const Vec2 w = Vec2{e.site.x, e.site.z} + Vec2{std::cos(a), std::sin(a * 1.3f)} * 2.2f;
                        const Vec2 to{w.x - it.position.x, w.y - it.position.z};
                        const f32 d = glm::length(to);
                        if (d > 0.05f) {
                            const f32 step = std::min(d, 0.6f * dt.seconds);
                            it.position.x += to.x / d * step;
                            it.position.z += to.y / d * step;
                            it.yaw = std::atan2(to.y, to.x);
                        }
                        it.position.y = ground_near(density, it.position.x, it.position.z, it.position.y, seed);
                    }
                }
                // A fight's foes all somehow gone (a debug clear) still counts.
                if ((e.kind == ErrandKind::Brigands || e.kind == ErrandKind::Wolves) && e.phase == QuestPhase::Active &&
                    std::none_of(ambush_.begin(), ambush_.end(), [&](const Enemy& en) { return en.quest == e.id; })) {
                    finish_errand(e);
                }
                break;
            }
            case QuestPhase::Complete:
                e.banner -= dt.seconds;
                break;
        }
        // The traveller turns to watch the nearest hero once they're close.
        if (near_giver < 8.0f) {
            for (const auto& [id, pl] : players_) {
                const Vec3 q = pl.controller.position();
                if (glm::length(Vec2{q.x - e.giver.position.x, q.z - e.giver.position.z}) <= near_giver + 0.01f) {
                    e.giver.yaw = std::atan2(q.z - e.giver.position.z, q.x - e.giver.position.x);
                }
            }
        }
    }

    // Offers walked away from are withdrawn; errands left far behind are dropped; a finished one goes once
    // its thanks are said and nobody's watching.
    std::vector<u32> gone;
    for (const Errand& e : errands_) {
        const f32 d = std::min(nearest_hero(players_, e.giver.position), nearest_hero(players_, e.site));
        if ((e.phase == QuestPhase::Offered && d > kErrandForget) || (e.phase == QuestPhase::Active && d > kErrandAbandon) ||
            (e.phase == QuestPhase::Complete && e.banner <= 0.0f && (d > 35.0f || e.banner < -45.0f))) {
            gone.push_back(e.id);
        }
    }
    for (const u32 id : gone) {
        std::erase_if(ambush_, [id](const Enemy& en) { return en.quest == id; });
        std::erase_if(quest_items_, [id](const QuestItem& it) { return it.quest == id; });
        std::erase_if(errands_, [id](const Errand& e) { return e.id == id; });
    }
}

} // namespace alryn
