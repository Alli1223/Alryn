// The living world: settlement tiers + street styles + the alpine plateaus' snowbound towns
// (Terrain/WorldGen.h, World/Village.h, World/PropLibrary snow twins), and life on the roads - the
// wayfarers, merchant caravans and roadside errands (Game/Wayfarer.h, Game/Wayfarers.cpp).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <Alryn/Game/Wayfarer.h>
#include <Alryn/Net/ByteBuffer.h>
#include <Alryn/Net/GameServer.h>
#include <Alryn/Net/NetClient.h>
#include <Alryn/Net/Protocol.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/World/Prop.h>
#include <Alryn/World/PropLibrary.h>
#include <Alryn/World/Village.h>

using namespace alryn;
using namespace alryn::net;

namespace {
std::vector<worldgen::Village> settlements(u32 seed, int radius) {
    std::vector<worldgen::Village> out;
    for (int vz = -radius; vz < radius; ++vz) {
        for (int vx = -radius; vx < radius; ++vx) {
            if (const auto v = worldgen::village_at(vx, vz, seed)) {
                out.push_back(*v);
            }
        }
    }
    return out;
}
usize house_count(const worldgen::Village& v, u32 seed) { return detail::cached_town_plan(v, seed).houses.size(); }
bool has_building(const worldgen::Village& v, u32 seed, u32 variant) {
    const auto& h = detail::cached_town_plan(v, seed).houses;
    return std::any_of(h.begin(), h.end(), [&](const detail::HousePlot& p) { return p.variant == variant; });
}

// A connected test client + a pump that ticks the server and drains the client's events.
struct Harness {
    GameServer server;
    NetClient client;
    PlayerId id = 0;
    Snapshot snap{};
    bool have_snap = false;
    PlayerInput intent{};

    bool start(u16 port, u32 seed) { return server.start(port, seed) && client.connect("127.0.0.1", port); }
    void pump(int iterations) {
        for (int i = 0; i < iterations; ++i) {
            client.send_input(intent);
            server.tick(Timestep{1.0f / 60.0f});
            for (const ClientEvent& e : client.poll(1)) {
                if (e.type == ClientEventType::WelcomeReceived) {
                    id = e.welcome.your_id;
                } else if (e.type == ClientEventType::SnapshotReceived) {
                    snap = e.snapshot;
                    have_snap = true;
                }
            }
        }
    }
    void place(const Vec3& p) {
        server.debug_place_player(id, Vec3{p.x, worldgen::height(p.x, p.z, server.seed()) + 0.5f, p.z});
        pump(4);
    }
    Vec3 me() const { return server.players().at(id).controller.position(); }
    // Out on the open road from the start town: `out` metres past its wall along the road to a neighbour.
    // Returns the road's direction there.
    Vec2 to_open_road(f32 out) {
        const Vec3 p = me();
        const auto town = worldgen::village_containing(p.x, p.z, server.seed(), 20.0f);
        REQUIRE(town.has_value());
        const auto dests = roads::reachable_towns(town->center, server.seed(), 1);
        REQUIRE_FALSE(dests.empty());
        const std::vector<Vec2> route = roads::route_polyline(town->center, dests.front().center, server.seed());
        f32 walked = 0.0f;
        const f32 want = town->half * 1.35f + out;
        for (usize i = 1; i < route.size(); ++i) {
            const f32 seg = glm::length(route[i] - route[i - 1]);
            if (walked + seg >= want) {
                const Vec2 q = glm::mix(route[i - 1], route[i], (want - walked) / seg);
                place(Vec3{q.x, 0.0f, q.y});
                return glm::normalize(route[i] - route[i - 1]);
            }
            walked += seg;
        }
        FAIL("the road is too short");
        return Vec2{1.0f, 0.0f};
    }
};
} // namespace

TEST_CASE("Settlements: hamlets of a few cottages up to great cities of hundreds of homes") {
    std::vector<worldgen::Village> all;
    for (const u32 seed : {4242u, 1u, 77u}) {
        for (const worldgen::Village& v : settlements(seed, 12)) {
            all.push_back(v);
            const usize n = house_count(v, seed);
            switch (v.tier) {
                case worldgen::TownTier::Hamlet:
                    CHECK(n <= 14u); // a handful of cottages round a well
                    CHECK(v.half < 30.0f);
                    break;
                case worldgen::TownTier::Village:
                    CHECK(n <= 40u);
                    break;
                case worldgen::TownTier::Town:
                    CHECK(n >= 6u);
                    break;
                case worldgen::TownTier::City:
                    CHECK(n >= 120u); // ring after ring of homes
                    CHECK(has_building(v, seed, kHouseKeep));
                    CHECK(has_building(v, seed, kHouseChapel));
                    CHECK(v.layout == worldgen::TownLayout::Rings);
                    break;
            }
        }
    }
    int tiers[4] = {};
    for (const worldgen::Village& v : all) {
        ++tiers[static_cast<int>(v.tier)];
    }
    CHECK(tiers[0] > 0);
    CHECK(tiers[1] > 0);
    CHECK(tiers[2] > 0);
    CHECK(tiers[3] > 0);
    // Varied street styles among the towns + villages (not every place is the same ring + spokes).
    bool grid = false, lane = false, radial = false;
    for (const worldgen::Village& v : all) {
        grid = grid || v.layout == worldgen::TownLayout::Grid;
        lane = lane || v.layout == worldgen::TownLayout::Lane;
        radial = radial || v.layout == worldgen::TownLayout::Radial;
    }
    CHECK(grid);
    CHECK(lane);
    CHECK(radial);
}

TEST_CASE("Settlements: a hamlet has no wall; a walled town's buildings never overlap or poke through it") {
    const u32 seed = 1u;
    int checked = 0;
    for (const worldgen::Village& v : settlements(seed, 10)) {
        const std::vector<PropInstance>& props = cached_village_props(v, seed);
        const bool walls = std::any_of(props.begin(), props.end(), [](const PropInstance& p) { return p.category == PropCategory::Wall; });
        CHECK(walls == worldgen::has_wall(v));
        const auto& houses = detail::cached_town_plan(v, seed).houses;
        std::vector<detail::Footprint> fps;
        for (const detail::HousePlot& hp : houses) {
            fps.emplace_back(hp.pos, hp.yaw, PropLibrary::house_half_extents(hp.variant));
        }
        for (usize i = 0; i < fps.size(); ++i) {
            for (usize j = i + 1; j < fps.size(); ++j) {
                CHECK_FALSE(detail::footprints_overlap(fps[i], fps[j], 0.1f));
            }
            for (const f32 sx : {-1.0f, 1.0f}) {
                for (const f32 sz : {-1.0f, 1.0f}) {
                    const Vec2 q = fps[i].corner(sx, sz) - v.center;
                    CHECK(glm::length(q) < worldgen::town_radius(v, std::atan2(q.y, q.x), seed));
                }
            }
        }
        if (++checked >= 8) {
            break;
        }
    }
    CHECK(checked > 0);
}

TEST_CASE("Settlements: the far-flung towns (and a city's broad lands) never crowd one another") {
    for (const u32 seed : {4242u, 77u}) {
        const std::vector<worldgen::Village> all = settlements(seed, 10);
        for (usize i = 0; i < all.size(); ++i) {
            for (usize j = i + 1; j < all.size(); ++j) {
                const f32 d = glm::length(all[i].center - all[j].center);
                CHECK(d > all[i].half * 1.35f + all[j].half * 1.35f + 10.0f);
            }
        }
    }
}

TEST_CASE("Settlements: the main streets follow the real road through town") {
    const u32 seed = 4242u;
    int towns = 0;
    for (const worldgen::Village& v : settlements(seed, 8)) {
        const detail::TownRoads& tr = detail::village_roads(v, seed);
        if (tr.paths.empty()) {
            continue;
        }
        ++towns;
        // Wherever a road runs through town (clear of the plaza), a street runs along it.
        for (const std::vector<Vec2>& path : tr.paths) {
            for (usize i = 1; i < path.size(); ++i) {
                const Vec2 probe = (path[i - 1] + path[i]) * 0.5f;
                const Vec2 d = probe - v.center;
                if (glm::length(d) < detail::ring_road_radius(v) + 2.0f ||
                    glm::length(d) > worldgen::town_radius(v, std::atan2(d.y, d.x), seed)) {
                    continue;
                }
                CHECK(town_street_distance(v, probe, seed) < 0.5f);
            }
        }
        if (towns >= 6) {
            break;
        }
    }
    CHECK(towns > 0);
}

TEST_CASE("Snow: the alpine plateaus hold snowbound towns, built snowy and lit by braziers") {
    std::optional<worldgen::Village> snowy;
    u32 seed = 0;
    for (const u32 s : {4242u, 1u, 77u, 9u}) {
        for (const worldgen::Village& v : settlements(s, 14)) {
            if (v.snowy && v.tier != worldgen::TownTier::Hamlet) {
                snowy = v;
                seed = s;
                break;
            }
        }
        if (snowy) {
            break;
        }
    }
    REQUIRE(snowy.has_value());
    CHECK(snowy->ground >= worldgen::snow_town_ground);
    CHECK(worldgen::snow_cover(snowy->ground) > 0.5f);
    CHECK(worldgen::biome_at(snowy->center.x, snowy->center.y, seed) == worldgen::Biome::Snow);
    int snow_houses = 0, plain_houses = 0, braziers = 0, snow_walls = 0;
    for (const PropInstance& p : cached_village_props(*snowy, seed)) {
        if (p.category == PropCategory::House) {
            (p.variant >= kSnowHouses ? snow_houses : plain_houses)++;
        } else if (p.category == PropCategory::Decor && p.variant == kDecorBrazier) {
            ++braziers;
        } else if (p.category == PropCategory::Wall && p.variant >= kSnowWalls) {
            ++snow_walls;
        }
    }
    CHECK(snow_houses > 0);
    CHECK(plain_houses == 0);
    CHECK(braziers > 0);
    CHECK(snow_walls > 0);
    // The snowy twins: the same building + a pale snow layer on top, a door lantern, a stoked hearth.
    PropLibrary lib(false);
    REQUIRE(lib.houses().size() == 2u * kHouseDefs);
    for (u32 i = 0; i < kHouseDefs; ++i) {
        const PropDef& base = lib.houses()[i];
        const PropDef& snow = lib.houses()[i + kSnowHouses];
        CHECK(snow.parts.size() > base.parts.size());
        CHECK(snow.lights.size() == base.lights.size() + 1u);
        CHECK(snow.colliders.size() == base.colliders.size());
    }
    CHECK(lib.walls().size() == 4u);
    CHECK(lib.gates().size() == 4u);
    CHECK(lib.markets().size() == 2u);
    CHECK_FALSE(lib.decor()[kDecorBrazier].lights.empty());
    CHECK_FALSE(lib.decor()[kDecorSnowman].colliders.empty());
}

TEST_CASE("Net: wayfarer roles, caravans and errands round-trip") {
    Snapshot s;
    VillagerState vs{77u, Vec3{1.0f, 2.0f, 3.0f}, 0.5f, 255, 5, 0, CharacterAppearance{}};
    vs.role = static_cast<u8>(WayfarerRole::Pilgrim);
    s.villagers.push_back(vs);
    s.quest_items.push_back({9u, Vec3{4.0f, 5.0f, 6.0f}, 2, 1, 1.25f});
    s.caravans.push_back({3u, Vec3{7.0f, 8.0f, 9.0f}, 0.3f, Vec3{10.0f, 11.0f, 12.0f}, 0.4f, 1, 1, 2});
    s.errands.push_back({0xE0000001u, 3, 1, 2, 3, 55u, 0xD0000001u, Vec3{1.0f}, Vec3{2.0f}});
    ByteWriter w;
    write(w, s);
    ByteReader r(w.bytes(), w.size());
    Snapshot back;
    REQUIRE(read(r, back));
    REQUIRE(back.villagers.size() == 1u);
    CHECK(back.villagers[0].role == static_cast<u8>(WayfarerRole::Pilgrim));
    CHECK(back.villagers[0].kind == 5u);
    REQUIRE(back.quest_items.size() == 1u);
    CHECK(back.quest_items[0].yaw == doctest::Approx(1.25f));
    REQUIRE(back.caravans.size() == 1u);
    CHECK(back.caravans[0].beast_pos.z == doctest::Approx(12.0f));
    CHECK(back.caravans[0].load == 2u);
    REQUIRE(back.errands.size() == 1u);
    CHECK(back.errands[0].kind == 3u);
    CHECK(back.errands[0].goal == 3u);
    CHECK(back.errands[0].giver_id == 0xD0000001u);
    CHECK(back.errands[0].site.x == doctest::Approx(2.0f));
}

TEST_CASE("GameServer: wayfarers + a merchant caravan walk the road between towns, keeping to it") {
    Harness h;
    REQUIRE(h.start(24841, 4242u));
    h.server.set_road_life(false);
    h.pump(30);
    REQUIRE(h.id != 0);
    h.to_open_road(60.0f);
    REQUIRE(h.server.debug_spawn_wayfarers(h.me(), true, 30.0f));
    REQUIRE(h.server.debug_spawn_wayfarers(h.me(), false, 18.0f));
    h.pump(30);
    REQUIRE(h.server.way_groups().size() == 2u);
    REQUIRE_FALSE(h.server.wayfarers().empty());
    std::vector<Vec3> before;
    for (const GameServer::Wayfarer& w : h.server.wayfarers()) {
        before.push_back(w.body.position);
    }
    h.pump(180); // three seconds of walking
    usize i = 0;
    for (const GameServer::Wayfarer& w : h.server.wayfarers()) {
        const Vec3 p = w.body.position;
        CHECK(roads::distance(p.x, p.z, h.server.seed()) < roads::road_half_width + 4.5f); // on / beside the road
        CHECK(glm::length(Vec2{p.x - before[i].x, p.z - before[i].z}) > 2.0f);              // ...walking it
        CHECK(std::abs(p.y - worldgen::height(p.x, p.z, h.server.seed())) < 1.0f);          // on the ground
        ++i;
    }
    // The client sees them: kind-5 villagers in their roles, and the caravan's cart + beast.
    REQUIRE(h.have_snap);
    CHECK(std::count_if(h.snap.villagers.begin(), h.snap.villagers.end(), [](const VillagerState& v) { return v.kind == 5; }) ==
          static_cast<long>(h.server.wayfarers().size()));
    REQUIRE_FALSE(h.snap.caravans.empty());
    CHECK(glm::length(h.snap.caravans[0].beast_pos - h.snap.caravans[0].position) > 2.0f);
    // Far away, they're gone.
    h.place(h.me() + Vec3{600.0f, 0.0f, 600.0f});
    h.pump(10);
    CHECK(h.server.way_groups().empty());
    CHECK(h.server.wayfarers().empty());
}

TEST_CASE("GameServer: on the open road a traveller in trouble turns up ahead with an errand") {
    Harness h;
    REQUIRE(h.start(24842, 4242u));
    h.pump(30);
    const Vec2 dir = h.to_open_road(55.0f);
    // Walk on down the road a while (the director waits a little way into a journey before the first).
    Vec3 p = h.me();
    for (int step = 0; step < 50 && h.server.errands().empty(); ++step) {
        const Vec2 next = Vec2{p.x, p.z} + dir * 1.2f;
        const auto snap = roads::nearest_point(next.x, next.y, h.server.seed());
        p = snap ? Vec3{snap->x, 0.0f, snap->y} : Vec3{next.x, 0.0f, next.y};
        h.place(p);
        h.pump(56);
    }
    REQUIRE_FALSE(h.server.errands().empty());
    const GameServer::Errand& e = h.server.errands().front();
    CHECK(e.phase == QuestPhase::Offered);
    // Waiting on the verge beside the road, a little way on, out of town.
    const Vec3 g = e.giver.position;
    CHECK(roads::distance(g.x, g.z, h.server.seed()) < roads::road_half_width + 5.0f);
    CHECK_FALSE(worldgen::inside_village(g.x, g.z, h.server.seed(), 5.0f));
    h.pump(6);
    REQUIRE(h.have_snap);
    REQUIRE(h.snap.errands.size() == 1u);
    CHECK(std::any_of(h.snap.villagers.begin(), h.snap.villagers.end(),
                      [&](const VillagerState& v) { return v.kind == 6 && v.id == h.snap.errands[0].giver_id; }));
}

TEST_CASE("GameServer: errand - find the runaway goat (E) and lead her home") {
    Harness h;
    REQUIRE(h.start(24843, 4242u));
    h.server.set_road_life(false);
    h.pump(30);
    const Vec2 dir = h.to_open_road(55.0f);
    REQUIRE(h.server.debug_spawn_errand(ErrandKind::LostGoat, h.me(), dir));
    const u32 id = h.server.errands().front().id;
    const Vec3 giver = h.server.errands().front().giver.position;
    const u32 money0 = h.server.money();
    // Talk to them (from beside them) - the goat is out there now.
    h.place(giver + Vec3{1.5f, 0.0f, 0.0f});
    h.intent.quest_pick = id;
    h.pump(4);
    h.intent.quest_pick = 0;
    REQUIRE(h.server.errands().front().phase == QuestPhase::Active);
    Vec3 goat_at{0.0f};
    bool found = false;
    for (const GameServer::QuestItem& it : h.server.quest_items()) {
        if (it.kind == 2u) {
            goat_at = it.position;
            found = true;
        }
    }
    REQUIRE(found);
    CHECK(glm::length(goat_at - giver) > 12.0f); // she ran off into the wilds
    // Walk up to her, take her by the rope...
    h.place(goat_at + Vec3{1.0f, 0.0f, 0.0f});
    h.intent.grab = true;
    h.pump(2);
    h.intent.grab = false;
    h.pump(2);
    u8 state = 0;
    for (const GameServer::QuestItem& it : h.server.quest_items()) {
        if (it.kind == 2u) {
            state = it.state;
        }
    }
    CHECK(state == 1u);
    // ...and walk her back in steps (she trots behind).
    Vec3 at = h.me();
    for (int i = 0; i < 40 && h.server.errands().front().phase == QuestPhase::Active; ++i) {
        const Vec3 to = giver - at;
        at += Vec3{to.x, 0.0f, to.z} * 0.15f;
        h.place(at);
        h.pump(30);
    }
    CHECK(h.server.errands().front().phase == QuestPhase::Complete);
    CHECK(h.server.money() > money0);
}

TEST_CASE("GameServer: errands - gather the satchels (E), dig the stuck cart free (Q), drive off brigands + wolves") {
    Harness h;
    REQUIRE(h.start(24844, 4242u));
    h.server.set_road_life(false);
    h.pump(30);
    const Vec2 dir = h.to_open_road(55.0f);
    const Vec3 road = h.me();
    auto take = [&](ErrandKind kind) {
        h.place(road);
        REQUIRE(h.server.debug_spawn_errand(kind, road, dir));
        const GameServer::Errand& e = h.server.errands().front();
        h.place(e.giver.position + Vec3{1.4f, 0.0f, 0.0f});
        h.intent.quest_pick = e.id;
        h.pump(4);
        h.intent.quest_pick = 0;
        REQUIRE(h.server.errands().front().phase == QuestPhase::Active);
        return h.server.errands().front().id;
    };

    // Satchels: three spilled back along the road; pick each up.
    {
        const u32 id = take(ErrandKind::LostParcels);
        std::vector<Vec3> bags;
        for (const GameServer::QuestItem& it : h.server.quest_items()) {
            if (it.quest == id && it.kind == 3u) {
                bags.push_back(it.position);
            }
        }
        REQUIRE(bags.size() == kParcelCount);
        for (const Vec3& b : bags) {
            h.place(b + Vec3{0.8f, 0.0f, 0.0f});
            h.intent.grab = true;
            h.pump(2);
            h.intent.grab = false;
            h.pump(2);
        }
        CHECK(h.server.errands().front().phase == QuestPhase::Complete);
    }
    // The stuck cart: spade strikes by its sunken wheel free it.
    {
        const u32 id = take(ErrandKind::StuckCart);
        Vec3 wheel{0.0f};
        bool found = false;
        for (const GameServer::QuestItem& it : h.server.quest_items()) {
            if (it.quest == id && it.kind == 4u) {
                wheel = it.position;
                found = true;
            }
        }
        REQUIRE(found);
        h.place(wheel + Vec3{2.0f, 0.0f, 0.0f});
        for (int i = 0; i < kStuckDigs + 1 && h.server.errands().front().phase == QuestPhase::Active; ++i) {
            h.intent.dig = true;
            h.intent.aim = wheel + Vec3{0.6f, 0.0f, 0.3f * static_cast<f32>(i)};
            h.pump(2);
            h.intent.dig = false;
            h.pump(40); // the spade's pace
        }
        CHECK(h.server.errands().front().phase == QuestPhase::Complete);
    }
    // Brigands + wolves: taking it on puts the foes out (in the hostile list).
    for (const ErrandKind kind : {ErrandKind::Brigands, ErrandKind::Wolves}) {
        take(kind);
        CHECK(h.server.ambusher_count() >= h.server.errands().front().goal);
    }
}
