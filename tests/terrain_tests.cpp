#include <doctest/doctest.h>
#include <chrono>
#include <thread>

#include <Alryn/Core/Noise.h>
#include <Alryn/Renderer/Vulkan/VulkanDevice.h>
#include <Alryn/Renderer/Vulkan/VulkanInstance.h>
#include <Alryn/Renderer/MeshPrimitives.h>
#include <Alryn/Terrain/MarchingTetra.h>
#include <Alryn/Terrain/StreamingTerrain.h>
#include <Alryn/Terrain/Terrain.h>
#include <Alryn/Terrain/PropScatter.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/TreeScatter.h>
#include <Alryn/Terrain/VegetationScatter.h>
#include <Alryn/Terrain/VoxelField.h>
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/World/PropLibrary.h>
#include <Alryn/World/Village.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

using namespace alryn;

namespace {
Vec3 white(const Vec3&, const Vec3&) {
    return Vec3{1.0f};
}
} // namespace

TEST_CASE("Noise: deterministic and bounded") {
    CHECK(noise::value2d(1.5f, 2.5f) == doctest::Approx(noise::value2d(1.5f, 2.5f)));
    for (int i = 0; i < 200; ++i) {
        const f32 n = noise::fbm2d(static_cast<f32>(i) * 0.3f, static_cast<f32>(i) * 0.7f);
        CHECK(n >= -1.01f);
        CHECK(n <= 1.01f);
    }
}

TEST_CASE("VoxelField: set/value/sample and out-of-range clamping") {
    VoxelField field(IVec3{10, 10, 10}, 1.0f, Vec3{0.0f});
    field.fill([](const Vec3&) { return 1.0f; }); // all air
    CHECK(field.value(5, 5, 5) == doctest::Approx(1.0f));

    field.set(5, 5, 5, -0.5f);
    CHECK(field.value(5, 5, 5) == doctest::Approx(-0.5f));
    CHECK(field.value(-3, 0, 0) == field.value(0, 0, 0)); // clamps coords

    field.fill([](const Vec3& p) { return p.x - 5.0f; }); // surface at x = 5
    CHECK(field.sample(Vec3{5.0f, 5.0f, 5.0f}) == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(field.sample(Vec3{6.0f, 5.0f, 5.0f}) > 0.0f);
}

TEST_CASE("VoxelField: apply_sphere carves and reports a region") {
    VoxelField field(IVec3{16, 16, 16}, 1.0f, Vec3{0.0f});
    field.fill([](const Vec3&) { return -1.0f; }); // all solid
    const VoxelField::Region region = field.apply_sphere(Vec3{8.0f, 8.0f, 8.0f}, 3.0f, 2.0f);
    CHECK(region.valid);
    CHECK(field.value(8, 8, 8) > -1.0f); // centre carved toward air
    CHECK(field.value(0, 0, 0) == doctest::Approx(-1.0f)); // far away untouched
}

TEST_CASE("VoxelField: raycast finds a flat surface") {
    VoxelField field(IVec3{20, 20, 20}, 0.5f, Vec3{0.0f});
    field.fill([](const Vec3& p) { return p.y - 3.0f; }); // solid below y = 3
    const auto hit = field.raycast(Vec3{2.0f, 8.0f, 2.0f}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->y == doctest::Approx(3.0f).epsilon(0.1));
}

TEST_CASE("MarchingTetra: uniform fields produce no geometry") {
    VoxelField solid(IVec3{8, 8, 8}, 1.0f);
    solid.fill([](const Vec3&) { return -1.0f; });
    CHECK(mc::polygonize(solid, IVec3{0}, solid.cell_count(), 0.0f, white).indices.empty());

    VoxelField air(IVec3{8, 8, 8}, 1.0f); // defaults to air
    CHECK(mc::polygonize(air, IVec3{0}, air.cell_count(), 0.0f, white).indices.empty());
}

TEST_CASE("MarchingTetra: a sphere is a closed, on-surface manifold") {
    VoxelField field(IVec3{24, 24, 24}, 0.5f, Vec3{-6.0f});
    const Vec3 center{0.0f};
    const f32 radius = 3.0f;
    field.fill([&](const Vec3& p) { return glm::length(p - center) - radius; });

    const MeshData mesh = mc::polygonize(field, IVec3{0}, field.cell_count(), 0.0f, white);
    REQUIRE(!mesh.vertices.empty());
    CHECK(mesh.indices.size() % 3 == 0);

    // Weld vertices by quantized position, then count how many triangles share
    // each edge. A closed manifold has every edge shared by exactly two.
    const f32 quant = field.voxel_size() * 0.001f;
    std::map<std::tuple<int, int, int>, int> ids;
    auto id_of = [&](const Vec3& p) {
        const std::tuple<int, int, int> key{static_cast<int>(std::lround(p.x / quant)),
                                            static_cast<int>(std::lround(p.y / quant)),
                                            static_cast<int>(std::lround(p.z / quant))};
        auto it = ids.find(key);
        if (it != ids.end()) {
            return it->second;
        }
        const int next = static_cast<int>(ids.size());
        ids.emplace(key, next);
        return next;
    };

    std::map<std::pair<int, int>, int> edge_count;
    auto add_edge = [&](int a, int b) {
        if (a > b) {
            std::swap(a, b);
        }
        ++edge_count[{a, b}];
    };
    for (usize i = 0; i < mesh.indices.size(); i += 3) {
        const int a = id_of(mesh.vertices[mesh.indices[i]].position);
        const int b = id_of(mesh.vertices[mesh.indices[i + 1]].position);
        const int c = id_of(mesh.vertices[mesh.indices[i + 2]].position);
        add_edge(a, b);
        add_edge(b, c);
        add_edge(c, a);
    }

    int boundary_edges = 0;
    for (const auto& [edge, count] : edge_count) {
        if (count != 2) {
            ++boundary_edges;
        }
    }
    CHECK(boundary_edges == 0); // watertight

    // Every vertex should sit on the sphere surface.
    for (const Vertex& v : mesh.vertices) {
        CHECK(glm::length(v.position - center) == doctest::Approx(radius).epsilon(0.1));
    }
}

TEST_CASE("Terrain: chunking, dirty tracking, and raycast (CPU)") {
    Terrain terrain(IVec3{33, 17, 33}, 0.5f, 16, Vec3{-8.0f, -4.0f, -8.0f});
    CHECK(terrain.chunk_count() == 2 * 1 * 2); // 32 cells / 16 -> 2 per x/z, 16/16 -> 1 y

    terrain.generate([](const Vec3& p) { return p.y; }); // surface at y = 0
    CHECK(terrain.any_dirty());

    const auto hit = terrain.raycast(Vec3{0.0f, 6.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->y == doctest::Approx(0.0f).epsilon(0.2));
}

TEST_CASE("Terrain: generate + rebuild meshes on a device (headless)") {
    vk::Instance instance;
    vk::InstanceConfig instance_config;
    instance_config.app_name = "alryn_terrain_test";
    if (!instance.create(instance_config)) {
        MESSAGE("No Vulkan instance - skipping terrain GPU test");
        return;
    }
    vk::Device device;
    if (!device.create(instance.handle())) {
        MESSAGE("No Vulkan device - skipping terrain GPU test");
        return;
    }

    Terrain terrain(IVec3{25, 17, 25}, 0.5f, 8, Vec3{-6.0f, -4.0f, -6.0f});
    terrain.generate([](const Vec3& p) { return p.y; }); // a flat-ish floor at y = 0
    CHECK(terrain.any_dirty());

    terrain.rebuild_dirty(device);
    CHECK_FALSE(terrain.any_dirty());

    int mesh_count = 0;
    terrain.for_each_mesh([&](const Mesh&) { ++mesh_count; });
    CHECK(mesh_count > 0); // the surface chunks produced geometry

    // Carving marks chunks dirty again.
    terrain.deform(Vec3{0.0f, 0.0f, 0.0f}, 2.0f, 2.0f);
    CHECK(terrain.any_dirty());
    terrain.rebuild_dirty(device);
    CHECK_FALSE(terrain.any_dirty());
}

TEST_CASE("Worldgen: biomes give smooth heights with both land and water") {
    const u32 seed = 1337u;
    const f32 h0 = worldgen::height(50.0f, 30.0f, seed);
    const f32 h1 = worldgen::height(50.2f, 30.0f, seed);
    CHECK(std::abs(h1 - h0) < 1.5f); // continuous

    bool has_land = false;
    bool has_water = false;
    for (int gz = 0; gz < 40; ++gz) {
        for (int gx = 0; gx < 40; ++gx) {
            const f32 x = static_cast<f32>(gx) * 18.0f - 360.0f;
            const f32 z = static_cast<f32>(gz) * 18.0f - 360.0f;
            const f32 h = worldgen::height(x, z, seed);
            if (h > worldgen::water_level + 1.0f) has_land = true;
            if (h < worldgen::water_level - 0.5f) has_water = true;
        }
    }
    CHECK(has_land);
    CHECK(has_water);

    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 peak = worldgen::surface_color(Vec3{0.0f, 13.0f, 0.0f}, up, seed);
    const Vec3 deep = worldgen::surface_color(Vec3{0.0f, -6.0f, 0.0f}, up, seed);
    CHECK(peak.r > deep.r); // snow brighter than wet silt
    for (const Vec3& c : {peak, deep}) {
        CHECK(c.r >= 0.0f);
        CHECK(c.r <= 1.0f);
        CHECK(c.g >= 0.0f);
        CHECK(c.g <= 1.0f);
        CHECK(c.b >= 0.0f);
        CHECK(c.b <= 1.0f);
    }
}

TEST_CASE("Trees: low-poly meshes + deterministic, on-land scatter") {
    const primitives::TreeMeshData pine = primitives::tree(0);
    const primitives::TreeMeshData round = primitives::tree(1);
    CHECK_FALSE(pine.trunk.indices.empty());
    CHECK_FALSE(pine.foliage.indices.empty());
    CHECK_FALSE(round.foliage.indices.empty());

    const u32 seed = 1337u;
    const auto a = scatter_trees(2, 3, 8.0f, seed);
    const auto b = scatter_trees(2, 3, 8.0f, seed);
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) {
        CHECK(a[i].position.x == doctest::Approx(b[i].position.x));
        CHECK(a[i].variant == b[i].variant);
    }

    int total = 0;
    bool underwater = false;
    for (int cz = -24; cz < 24; ++cz) {
        for (int cx = -24; cx < 24; ++cx) {
            for (const TreeInstance& t : scatter_trees(cx, cz, 8.0f, seed)) {
                ++total;
                if (t.position.y < worldgen::water_level) {
                    underwater = true;
                }
                CHECK(t.scale > 0.5f);
                CHECK((t.variant >= 0 && t.variant < 5)); // pine/oak/birch/broad/dead
            }
        }
    }
    CHECK(total > 0);
    CHECK_FALSE(underwater);
}

TEST_CASE("Vegetation: grass + flowers bake deterministically onto land") {
    CHECK_FALSE(primitives::grass_tuft().indices.empty());
    CHECK_FALSE(primitives::flower().indices.empty());

    const u32 seed = 1337u;
    const MeshData a = build_vegetation(2, 3, 8.0f, seed);
    const MeshData b = build_vegetation(2, 3, 8.0f, seed);
    REQUIRE(a.vertices.size() == b.vertices.size());
    REQUIRE(a.indices.size() == b.indices.size());

    usize total_indices = 0;
    f32 min_y = 1e9f;
    bool any = false;
    for (int cz = -16; cz < 16; ++cz) {
        for (int cx = -16; cx < 16; ++cx) {
            const MeshData v = build_vegetation(cx, cz, 8.0f, seed);
            total_indices += v.indices.size();
            for (const Vertex& vert : v.vertices) {
                any = true;
                min_y = std::min(min_y, vert.position.y);
            }
        }
    }
    CHECK(total_indices > 0);
    REQUIRE(any);
    CHECK(min_y >= worldgen::water_level); // nothing grows below the waterline
}

TEST_CASE("Props: library builds geometry, scatter is deterministic & on land") {
    PropLibrary lib;
    REQUIRE(lib.bushes().size() >= 1);
    REQUIRE(lib.rocks().size() >= 1);
    REQUIRE(lib.logs().size() >= 1);
    CHECK_FALSE(lib.bushes()[0].parts.empty());
    CHECK_FALSE(lib.rocks()[0].parts.empty());

    // Logs are solid (have a collider); bushes/rocks are decorative.
    for (const PropDef& log : lib.logs()) {
        CHECK_FALSE(log.parts.empty());
        CHECK_FALSE(log.colliders.empty());
    }
    CHECK(lib.bushes()[0].colliders.empty());

    const u32 seed = 1337u;
    const auto a = scatter_props(1, 2, 8.0f, seed);
    const auto b = scatter_props(1, 2, 8.0f, seed);
    REQUIRE(a.size() == b.size());

    int bushes = 0, rocks = 0, logs = 0;
    bool underwater = false;
    for (int cz = -20; cz < 20; ++cz) {
        for (int cx = -20; cx < 20; ++cx) {
            for (const PropInstance& p : scatter_props(cx, cz, 8.0f, seed)) {
                if (p.position.y < worldgen::water_level) underwater = true;
                if (p.category == PropCategory::Bush) ++bushes;
                else if (p.category == PropCategory::Rock) ++rocks;
                else if (p.category == PropCategory::Log) ++logs;
            }
        }
    }
    CHECK(bushes > 0);
    CHECK(rocks > 0);
    CHECK(logs > 0);
    CHECK_FALSE(underwater);

    // Standalone wild lanterns must never stack on top of each other: each global grid cell is
    // owned by exactly one chunk, so the 23 m lantern grid (over 8 m chunks) is no longer emitted
    // by several neighbouring chunks at the same spot (which multiplied their light 3x3 = far too
    // bright). Gather grid lanterns (off the roads, which have their own posts) and check spacing.
    std::vector<Vec3> lanterns;
    for (int cz = -20; cz < 20; ++cz) {
        for (int cx = -20; cx < 20; ++cx) {
            for (const PropInstance& p : scatter_props(cx, cz, 8.0f, seed)) {
                if (p.category == PropCategory::Lantern &&
                    roads::distance(p.position.x, p.position.z, seed) > roads::road_half_width + 2.0f) {
                    lanterns.push_back(p.position);
                }
            }
        }
    }
    REQUIRE(lanterns.size() > 4); // there are wild lanterns in this region
    int stacked = 0;
    for (usize i = 0; i < lanterns.size(); ++i) {
        for (usize j = i + 1; j < lanterns.size(); ++j) {
            if (glm::length(lanterns[i] - lanterns[j]) < 0.5f) {
                ++stacked;
            }
        }
    }
    CHECK(stacked == 0); // none piled on top of another
}

TEST_CASE("Paths: fences + lanterns line the trail edges; lanterns glow + light") {
    PropLibrary lib;
    REQUIRE_FALSE(lib.fences().empty());
    REQUIRE_FALSE(lib.lanterns().empty());
    CHECK_FALSE(lib.fences()[0].colliders.empty()); // fences block you

    // A lantern has an emissive part (glass) and a spot light.
    const PropDef& lantern = lib.lanterns()[0];
    CHECK_FALSE(lantern.lights.empty());
    bool has_emissive = false;
    for (const PropPart& part : lantern.parts) {
        if (part.layer == PropLayer::Emissive && !part.mesh.indices.empty()) has_emissive = true;
    }
    CHECK(has_emissive);

    // Fences line the edges of the inter-town roads; lanterns appear both along the
    // roads and as standalone world lanterns dotted off them.
    const u32 seed = 4242u;
    const auto segs = roads::gather(Vec2{0.0f, 0.0f}, 1400.0f, seed);
    REQUIRE_FALSE(segs.empty()); // towns are connected by routed roads

    // A dry, gentle stretch of road to scan around.
    Vec2 rp{0.0f};
    bool found_rp = false;
    for (const roads::Segment& s : segs) {
        const Vec2 mid = (s.a + s.b) * 0.5f;
        if (worldgen::height(mid.x, mid.y, seed) > worldgen::water_level + 1.0f) {
            rp = mid;
            found_rp = true;
            break;
        }
    }
    REQUIRE(found_rp);

    auto on_edge = [&](const PropInstance& p) {
        return std::abs(roads::distance(p.position.x, p.position.z, seed) - roads::road_half_width) <
               0.7f;
    };
    const f32 cw = 8.0f;
    const int c0x = static_cast<int>(std::floor(rp.x / cw));
    const int c0z = static_cast<int>(std::floor(rp.y / cw));
    int fences = 0, fences_off_edge = 0;
    std::vector<Vec2> post_pos;
    std::vector<PropInstance> rails;
    for (int cz = -8; cz <= 8; ++cz) {
        for (int cx = -8; cx <= 8; ++cx) {
            for (const PropInstance& p : scatter_props(c0x + cx, c0z + cz, cw, seed)) {
                if (p.category == PropCategory::Fence) {
                    ++fences;
                    if (!on_edge(p)) ++fences_off_edge;
                    post_pos.emplace_back(p.position.x, p.position.z);
                } else if (p.category == PropCategory::FenceRail) {
                    rails.push_back(p);
                }
            }
        }
    }
    CHECK(fences > 0);
    CHECK(fences_off_edge == 0); // fences hug the road edge

    // Posts are now joined by RAILS stretched to the exact gap (varying length), forming a
    // post-and-rail run. A rail spans from its centre out ±length/2 along its yaw; check
    // that (almost) every rail lands a post at BOTH ends - i.e. they connect posts up.
    REQUIRE(rails.size() > 6);
    int connected = 0;
    for (const PropInstance& r : rails) {
        CHECK(r.length > 0.4f); // a real, stretched span
        const Vec2 mid{r.position.x, r.position.z};
        const Vec2 dir{std::cos(r.yaw), -std::sin(r.yaw)}; // inverse of atan2(-dir.y,dir.x)
        const Vec2 e0 = mid - dir * (r.length * 0.5f);
        const Vec2 e1 = mid + dir * (r.length * 0.5f);
        auto post_near = [&](const Vec2& e) {
            for (const Vec2& pp : post_pos) {
                if (glm::length(pp - e) < 0.5f) return true;
            }
            return false;
        };
        if (post_near(e0) && post_near(e1)) ++connected;
    }
    CHECK(connected >= static_cast<int>(rails.size()) - 4); // essentially every rail links two posts

    // Standalone world lanterns are dotted off the roads somewhere in the wider world.
    int world_lanterns = 0;
    for (int cz = -24; cz < 24; ++cz) {
        for (int cx = -24; cx < 24; ++cx) {
            for (const PropInstance& p : scatter_props(cx, cz, cw, seed)) {
                if (p.category == PropCategory::Lantern &&
                    roads::distance(p.position.x, p.position.z, seed) > roads::road_half_width + 2.0f) {
                    ++world_lanterns;
                }
            }
        }
    }
    CHECK(world_lanterns > 0);
}

TEST_CASE("Village: medieval cottage / wall / gate building blocks") {
    PropLibrary lib;
    REQUIRE_FALSE(lib.houses().empty());
    REQUIRE_FALSE(lib.walls().empty());
    REQUIRE_FALSE(lib.gates().empty());

    // There are several house variants (cottages, longhouses, two-storey, manors...).
    CHECK(lib.houses().size() == kHouseVariants);
    // Each: a roof/shell part (fades when inside) + emissive (hearth fire / candle / lamp
    // glow) + a real footprint, interior lights, a bed spot inside, and no fake glow.
    for (const PropDef& house : lib.houses()) {
        CHECK(house.footprint.x > 0.0f);
        CHECK_FALSE(house.colliders.empty());
        bool has_roof = false, has_fire = false;
        for (const PropPart& part : house.parts) {
            if (part.layer == PropLayer::Roof && !part.mesh.indices.empty()) has_roof = true;
            if (part.layer == PropLayer::Emissive && !part.mesh.indices.empty()) has_fire = true;
            CHECK(part.layer != PropLayer::Glow);
        }
        CHECK(has_roof);
        CHECK(has_fire);
        CHECK(house.lights.size() >= 2);
        // The resident's bed spot sits inside the house footprint (so they sleep indoors).
        CHECK(std::abs(house.bed_spot.x) < house.footprint.x);
        CHECK(std::abs(house.bed_spot.z) < house.footprint.y);
    }
    // Two-storey variants are taller than single-storey ones.
    CHECK(lib.houses()[2].wall_height > lib.houses()[0].wall_height);
    // Wall blocks; gate tower has a brazier light.
    CHECK_FALSE(lib.walls()[0].colliders.empty());
    CHECK_FALSE(lib.gates()[0].lights.empty());
    // The well exists as a solid, collidable structure (the firefighting water source).
    REQUIRE_FALSE(lib.wells().empty());
    CHECK_FALSE(lib.wells()[0].colliders.empty());
    CHECK_FALSE(lib.wells()[0].parts.empty());
}

TEST_CASE("Village: towns are placed, laid out deterministically, with houses + gates") {
    const u32 seed = 4242u;
    // Find a town somewhere on the village grid.
    std::optional<worldgen::Village> found;
    for (int vz = -8; vz < 8 && !found; ++vz) {
        for (int vx = -8; vx < 8 && !found; ++vx) {
            found = worldgen::village_at(vx, vz, seed);
        }
    }
    REQUIRE(found.has_value());
    CHECK(found->ground >= worldgen::water_level + 2.0f);
    CHECK(worldgen::inside_village(found->center.x, found->center.y, seed));
    CHECK_FALSE(worldgen::inside_village(found->center.x + 400.0f, found->center.y, seed));

    const auto props = village_props(*found, seed);
    const auto props2 = village_props(*found, seed);
    REQUIRE(props.size() == props2.size()); // deterministic
    int houses = 0, walls = 0, gates = 0, markets = 0;
    Vec3 market_pos{0.0f};
    std::vector<Vec3> house_pos;
    for (const PropInstance& p : props) {
        if (p.category == PropCategory::House) { ++houses; house_pos.push_back(p.position); }
        else if (p.category == PropCategory::Wall) ++walls;
        else if (p.category == PropCategory::Gate) ++gates;
        else if (p.category == PropCategory::Market) { ++markets; market_pos = p.position; }
    }
    CHECK(houses >= 4);
    // Houses never spawn on top of each other (overlap rejection keeps them apart)...
    for (usize i = 0; i < house_pos.size(); ++i) {
        for (usize j = i + 1; j < house_pos.size(); ++j) {
            const Vec2 d{house_pos[i].x - house_pos[j].x, house_pos[i].z - house_pos[j].z};
            CHECK(glm::length(d) >= 6.0f);
        }
        // ...and they stay inside the town's (organic) wall, never poking through it.
        const Vec2 d{house_pos[i].x - found->center.x, house_pos[i].z - found->center.y};
        CHECK(glm::length(d) < worldgen::town_radius(*found, std::atan2(d.y, d.x), seed));
    }
    CHECK(walls >= 8);
    CHECK(gates >= 4);
    // Wagon depot spots are reserved clear of every house (so carts never spawn in one).
    const auto wagon_spots = village_wagon_spots(*found, seed);
    REQUIRE_FALSE(wagon_spots.empty());
    for (const Vec3& s : wagon_spots) {
        for (const Vec3& h : house_pos) {
            CHECK(glm::length(Vec2{s.x - h.x, s.z - h.z}) > 3.0f);
        }
    }
    // A gate on every side a road enters from (1-4); never more than the four sides.
    const usize gate_count = village_gates(*found, seed).size();
    CHECK(gate_count >= 1);
    CHECK(gate_count <= 4);
    // Organic outline: the radial boundary stays within sane bounds at every angle.
    for (int k = 0; k < 16; ++k) {
        const f32 r = worldgen::town_radius(*found, TwoPi * static_cast<f32>(k) / 16.0f, seed);
        CHECK(r >= found->half * 0.6f - 0.01f);
        CHECK(r <= found->half * 1.35f + 0.01f);
    }
    // Exactly one marketplace, sitting at the plaza centre (the town's heart).
    CHECK(markets == 1);
    CHECK(market_pos.x == doctest::Approx(found->center.x));
    CHECK(market_pos.z == doctest::Approx(found->center.y));

    // No tree spawns inside the town walls.
    bool tree_in_town = false;
    const int cw = 8;
    for (int cz = -10; cz < 10; ++cz) {
        for (int cx = -10; cx < 10; ++cx) {
            const int bx = static_cast<int>(std::floor(found->center.x / cw)) + cx;
            const int bz = static_cast<int>(std::floor(found->center.y / cw)) + cz;
            for (const TreeInstance& t : scatter_trees(bx, bz, 8.0f, seed)) {
                const Vec2 d{t.position.x - found->center.x, t.position.z - found->center.y};
                if (glm::length(d) < worldgen::town_radius(*found, std::atan2(d.y, d.x), seed)) {
                    tree_in_town = true; // inside the town's organic wall
                }
            }
        }
    }
    CHECK_FALSE(tree_in_town);
}

TEST_CASE("Roads: routed roads connect towns, avoid water, and trees keep off them") {
    const u32 seed = 4242u;
    const auto segs = roads::gather(Vec2{0.0f, 0.0f}, 1400.0f, seed);
    REQUIRE_FALSE(segs.empty()); // towns are linked by roads

    // A point on a road has distance ~ 0...
    const Vec2 rp = (segs[0].a + segs[0].b) * 0.5f;
    CHECK(roads::distance(rp.x, rp.y, seed) < 0.6f);
    // ...and the road never dips into the water (routing bends around it).
    for (const roads::Segment& s : segs) {
        CHECK(worldgen::height(s.a.x, s.a.y, seed) >= worldgen::water_level + 0.3f);
        CHECK(worldgen::height(s.b.x, s.b.y, seed) >= worldgen::water_level + 0.3f);
    }

    // No tree sits on a road centre line near that stretch of road.
    const f32 cw = 8.0f;
    const int c0x = static_cast<int>(std::floor(rp.x / cw));
    const int c0z = static_cast<int>(std::floor(rp.y / cw));
    bool tree_on_road = false;
    for (int cz = -8; cz <= 8; ++cz) {
        for (int cx = -8; cx <= 8; ++cx) {
            for (const TreeInstance& t : scatter_trees(c0x + cx, c0z + cz, cw, seed)) {
                if (roads::distance(t.position.x, t.position.z, seed) < roads::road_half_width) {
                    tree_on_road = true;
                }
            }
        }
    }
    CHECK_FALSE(tree_on_road);
}

TEST_CASE("Roads: route_polyline links two connected towns, ordered and above water") {
    const u32 seed = 4242u;
    // Find a road-connected pair of towns.
    std::optional<worldgen::Village> a, b;
    for (int vz = -6; vz <= 6 && !b; ++vz) {
        for (int vx = -6; vx <= 6 && !b; ++vx) {
            const auto va = worldgen::village_at(vx, vz, seed);
            if (!va) {
                continue;
            }
            for (int dz = -roads::road_max_cells; dz <= roads::road_max_cells && !b; ++dz) {
                for (int dx = -roads::road_max_cells; dx <= roads::road_max_cells && !b; ++dx) {
                    if (dx == 0 && dz == 0) {
                        continue;
                    }
                    const auto vb = worldgen::village_at(vx + dx, vz + dz, seed);
                    if (vb && !roads::route_polyline(va->center, vb->center, seed).empty()) {
                        a = va;
                        b = vb;
                    }
                }
            }
        }
    }
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    const std::vector<Vec2> poly = roads::route_polyline(a->center, b->center, seed);
    REQUIRE(poly.size() >= 2);
    CHECK(glm::length(poly.front() - a->center) < 1.0f); // starts at the source
    CHECK(glm::length(poly.back() - b->center) < 1.0f);  // ends at the destination
    for (const Vec2& p : poly) {
        CHECK(worldgen::height(p.x, p.y, seed) >= worldgen::water_level + 0.3f); // never in water
    }
}

TEST_CASE("Vegetation: a chunk bakes a dense, varied ground mesh; deterministic") {
    const u32 seed = 4242u;
    const f32 cw = 8.0f;
    // Find a chunk that grows vegetation (most land does) and confirm it's rich.
    bool found_rich = false;
    for (int cz = 0; cz < 24 && !found_rich; ++cz) {
        for (int cx = 0; cx < 24 && !found_rich; ++cx) {
            const MeshData veg = build_vegetation(cx, cz, cw, seed);
            const MeshData veg2 = build_vegetation(cx, cz, cw, seed);
            REQUIRE(veg.vertices.size() == veg2.vertices.size()); // deterministic
            if (veg.vertices.size() > 800) { // many plants baked into one mesh
                found_rich = true;
            }
        }
    }
    CHECK(found_rich);
}

TEST_CASE("StreamingTerrain: streams + unloads chunks around a moving focus") {
    vk::Instance instance;
    vk::InstanceConfig instance_config;
    instance_config.app_name = "alryn_streaming_test";
    if (!instance.create(instance_config)) {
        MESSAGE("No Vulkan instance - skipping streaming terrain test");
        return;
    }
    vk::Device device;
    if (!device.create(instance.handle())) {
        MESSAGE("No Vulkan device - skipping streaming terrain test");
        return;
    }

    StreamingTerrain terrain(123u, 0.5f, 16, 2); // view radius 2 -> up to 5x5 chunks

    // Chunks are generated on a background thread, so pump update() until the worker
    // has filled in enough around the focus (with a generous frame cap so the test is
    // robust to worker throughput / system load).
    auto pump_until = [&](const Vec3& focus, usize target, int max_frames) {
        for (int i = 0; i < max_frames && terrain.loaded_chunk_count() < target; ++i) {
            terrain.update(focus, device);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    };
    pump_until(Vec3{0.0f, 0.0f, 0.0f}, 9, 600);
    CHECK(terrain.loaded_chunk_count() >= 9);
    int meshes = 0;
    terrain.for_each_mesh([&](const Mesh&) { ++meshes; });
    CHECK(meshes > 0); // the surface passes through every column chunk

    // The world is infinite: raycast far from the origin still finds ground.
    const auto far_hit = terrain.raycast(Vec3{800.0f, 30.0f, 800.0f}, Vec3{0.0f, -1.0f, 0.0f}, 60.0f);
    CHECK(far_hit.has_value());

    // Move the focus far away; old chunks unload, new ones stream in.
    const Vec3 far{800.0f, 0.0f, 800.0f};
    for (int i = 0; i < 5; ++i) {
        terrain.update(far, device); // evict the origin chunks (out of range)
    }
    pump_until(far, 9, 600);
    CHECK(terrain.loaded_chunk_count() >= 9);
    CHECK(terrain.loaded_chunk_count() <= 64); // bounded - origin chunks were evicted

    int meshes_far = 0;
    terrain.for_each_mesh([&](const Mesh&) { ++meshes_far; });
    CHECK(meshes_far > 0);
}
