#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Noise.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Terrain/ScatterHash.h>

#include <cmath>
#include <optional>

// Deterministic world description shared by the server and every client. Terrain
// is a biome-blended height field; clients and the server sample the same
// functions, so only runtime edits travel over the network.
namespace alryn::worldgen {

inline constexpr f32 voxel_size = 0.5f;
inline constexpr f32 water_level = -1.0f;

// Smooth surface height at (x, z): blends ocean / lowland / mountains from a
// low-frequency "region" field, then layers rolling hills + finer detail on land
// for a wild, undulating forested landscape.
inline f32 height(f32 x, f32 z, u32 seed) {
    const f32 region = noise::fbm2d(x * 0.006f, z * 0.006f, 3, 2.0f, 0.5f, seed + 101u);
    const f32 continental = glm::smoothstep(-0.30f, 0.10f, region); // 0 ocean .. 1 land
    const f32 mountainous = glm::smoothstep(0.35f, 0.90f, region);  // 0 .. 1 mountains

    const f32 base = glm::mix(-8.0f, 1.0f, continental) + mountainous * 7.0f;

    // Big rolling hills over all land (gentle, walkable, immersive undulation).
    const f32 hills = noise::fbm2d(x * 0.017f, z * 0.017f, 4, 2.0f, 0.5f, seed + 7u);
    // Mid-frequency terrain shape (bumps, dells) + fine surface roughness.
    const f32 detail = noise::fbm2d(x * 0.045f, z * 0.045f, 5, 2.0f, 0.5f, seed);
    const f32 fine = noise::fbm2d(x * 0.14f, z * 0.14f, 3, 2.0f, 0.5f, seed + 9u);

    const f32 land_amp = glm::mix(2.2f, 9.0f, mountainous) * continental + 0.4f;
    return base + continental * hills * 2.6f + land_amp * (detail * 0.7f + fine * 0.22f);
}

inline f32 density(const Vec3& p, u32 seed) {
    return p.y - height(p.x, p.z, seed);
}

// Wetness field: drives lush forest (wet) vs open meadow / dry sand (dry). Biased
// positive so most of the land reads as green woodland, with drier clearings.
inline f32 moisture(f32 x, f32 z, u32 seed) {
    return noise::fbm2d(x * 0.018f, z * 0.018f, 3, 2.0f, 0.5f, seed + 202u) + 0.14f;
}

// Roads connecting nearby towns (routed to avoid water) live in Terrain/RoadNetwork.h.
// The old noise-contour "paths" are gone; road colouring is overlaid while meshing
// (roads::tint_surface) so this header stays free of the road-network dependency.

// Local terrain slope at (x,z) (how much it tilts over ~1 unit). Shared by the
// scatters and the road-flatness gate.
inline f32 slope(f32 x, f32 z, u32 seed) {
    const f32 g = height(x, z, seed);
    return std::abs(height(x + 1.0f, z, seed) - g) + std::abs(height(x, z + 1.0f, seed) - g);
}

// --- Villages -------------------------------------------------------------
// Medieval towns sit on flat, above-water ground, sparsely scattered on a coarse
// grid. The placement lives here (it only needs the height field + hash) so terrain
// colouring and the scatters can ask "am I in a village?"; the actual buildings are
// laid out in World/Village.h.
inline constexpr f32 village_cell = 170.0f;     // grid spacing of candidate towns
inline constexpr f32 village_half = 38.0f;      // half-width of a typical (medium) town
inline constexpr f32 village_half_max = 44.0f;  // a large town's half-width

struct Village {
    Vec2 center{0.0f}; // xz of the town centre
    f32 ground = 0.0f; // ground height at the centre
    f32 half = village_half;
    u32 vseed = 0; // per-village layout seed
};

// The town whose origin falls in coarse cell (vcx,vcz), if the ground suits it. Towns
// come in three sizes (small/medium/large), chosen per cell; a bigger town needs a
// wider patch of flat ground, so large towns are rarer.
inline std::optional<Village> village_at(int vcx, int vcz, u32 seed) {
    const u32 salt = seed + 313u;
    if (detail::hash01(detail::tree_hash(vcx, vcz, salt)) > 0.5f) {
        return std::nullopt; // only some cells grow a town
    }
    const f32 sz = detail::hash01(detail::tree_hash(vcx, vcz, salt + 8u));
    // Sprawling medieval towns: small / medium / large half-widths. A bigger town needs a
    // wider patch of buildable ground, so large towns are rarer. The max is capped so two
    // adjacent towns can never overlap at the grid spacing + jitter.
    const f32 half = sz < 0.45f ? 30.0f : (sz < 0.82f ? 38.0f : 44.0f);
    const f32 jx = (detail::hash01(detail::tree_hash(vcx, vcz, salt + 1u)) - 0.5f) * village_cell * 0.3f;
    const f32 jz = (detail::hash01(detail::tree_hash(vcx, vcz, salt + 2u)) - 0.5f) * village_cell * 0.3f;
    const f32 cx = (static_cast<f32>(vcx) + 0.5f) * village_cell + jx;
    const f32 cz = (static_cast<f32>(vcz) + 0.5f) * village_cell + jz;
    const f32 gh = height(cx, cz, seed);
    if (gh < water_level + 2.0f || gh > 7.0f) {
        return std::nullopt;
    }
    for (f32 ox : {-half, 0.0f, half}) {
        for (f32 oz : {-half, 0.0f, half}) {
            if (std::abs(height(cx + ox, cz + oz, seed) - gh) > 2.8f) {
                return std::nullopt; // not flat enough for a town of this size
            }
        }
    }
    return Village{Vec2{cx, cz}, gh, half, detail::tree_hash(vcx, vcz, salt + 7u)};
}

// Distance from a town's centre to its wall at world-angle `ang`. Each town has its own
// shape (from its vseed): a blend of round and square, modulated by low-frequency angular
// lumps and an occasional protrusion (a sticky-out bit), so towns aren't all square boxes.
// This one function drives the inside-test, the walls, the gates and house placement, so
// they all agree on the outline.
inline f32 town_radius(const Village& v, f32 ang, u32 seed) {
    const int vid = static_cast<int>(v.vseed);
    const f32 c = std::abs(std::cos(ang));
    const f32 s = std::abs(std::sin(ang));
    const f32 square = v.half / std::max(std::max(c, s), 0.5f); // square boundary at this angle
    const f32 squareness = detail::hash01(detail::tree_hash(vid, 0, 4242u));
    f32 r = glm::mix(v.half, square * 0.92f, squareness);
    // Low-frequency angular lumps (periodic in angle because it samples on the unit circle).
    const f32 lump = noise::fbm2d(std::cos(ang) * 1.5f + static_cast<f32>(vid % 97u),
                                  std::sin(ang) * 1.5f, 2, 2.0f, 0.5f, seed + 555u);
    r *= 1.0f + 0.12f * lump;
    // ~half of towns grow a protrusion (a bump) at a hashed angle.
    if (detail::hash01(detail::tree_hash(vid, 1, 4243u)) < 0.5f) {
        const f32 a0 = detail::hash01(detail::tree_hash(vid, 2, 4244u)) * TwoPi;
        f32 da = ang - a0;
        while (da > Pi) da -= TwoPi;
        while (da < -Pi) da += TwoPi;
        constexpr f32 width = 0.5f;
        r += v.half * 0.32f * std::exp(-(da * da) / (2.0f * width * width));
    }
    return glm::clamp(r, v.half * 0.6f, v.half * 1.35f);
}

// The nearest town containing (x,z) within `margin` of its (organic) outline, if any.
inline std::optional<Village> village_containing(f32 x, f32 z, u32 seed, f32 margin = 0.0f) {
    const int vcx = static_cast<int>(std::floor(x / village_cell));
    const int vcz = static_cast<int>(std::floor(z / village_cell));
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (const auto v = village_at(vcx + dx, vcz + dz, seed)) {
                const Vec2 d{x - v->center.x, z - v->center.y};
                const f32 dist = glm::length(d);
                if (dist < town_radius(*v, std::atan2(d.y, d.x), seed) + margin) {
                    return v;
                }
            }
        }
    }
    return std::nullopt;
}
inline bool inside_village(f32 x, f32 z, u32 seed, f32 margin = 0.0f) {
    return village_containing(x, z, seed, margin).has_value();
}

// Surface colour from height, slope, and moisture -> beaches, grass/desert,
// rocky slopes, and snow peaks.
inline Vec3 surface_color(const Vec3& p, const Vec3& normal, u32 seed) {
    const f32 up = glm::clamp(normal.y, 0.0f, 1.0f);
    const f32 m = moisture(p.x, p.z, seed);
    const f32 h = p.y;

    const Vec3 grass{0.22f, 0.40f, 0.19f};  // deep forest green
    const Vec3 grass2{0.31f, 0.47f, 0.23f}; // lighter clearing green
    const Vec3 dirt{0.33f, 0.26f, 0.17f};   // leaf litter / bare soil
    const Vec3 sand{0.78f, 0.71f, 0.47f};
    const Vec3 wet_sand{0.42f, 0.40f, 0.32f};
    const Vec3 rock{0.42f, 0.42f, 0.46f};
    const Vec3 snow{0.92f, 0.93f, 0.97f};

    // Forest floor: blend two greens by a mid-frequency field, with patches of
    // bare earth / leaf-litter, so the ground reads varied rather than flat green.
    const f32 tone = noise::fbm2d(p.x * 0.06f, p.z * 0.06f, 2, 2.0f, 0.5f, seed + 303u);
    const f32 litter = noise::fbm2d(p.x * 0.11f, p.z * 0.11f, 2, 2.0f, 0.5f, seed + 404u);
    Vec3 ground = glm::mix(grass, grass2, glm::smoothstep(-0.25f, 0.25f, tone));
    ground = glm::mix(ground, dirt, glm::smoothstep(0.22f, 0.5f, litter));
    // Flat ground: desert sand <-> forest floor by moisture.
    Vec3 flat = glm::mix(sand, ground, glm::smoothstep(-0.15f, 0.2f, m));
    // Steep slopes turn rocky.
    Vec3 color = glm::mix(rock, flat, glm::smoothstep(0.5f, 0.75f, up));

    // Sandy beach band just above the water on gentle ground.
    const f32 beach = glm::smoothstep(water_level + 2.2f, water_level + 0.3f, h) *
                      glm::smoothstep(0.6f, 0.85f, up);
    color = glm::mix(color, sand, beach);
    // Darker silt below the waterline.
    color = glm::mix(color, wet_sand, glm::smoothstep(water_level + 0.2f, water_level - 2.5f, h));
    // Snow on high, flatter ground.
    const f32 snow_amt = glm::smoothstep(8.5f, 12.5f, h) * glm::smoothstep(0.5f, 0.8f, up);
    color = glm::mix(color, snow, snow_amt);

    // (The worn dirt road colour is overlaid separately via roads::tint_surface while
    // meshing, so this base colour stays independent of the road network.)

    // Trampled muddy earth inside a town's walls: dirty churned mud blotched with patches of
    // worn dirt-grey, much dirtier than clean cobble (the streets are laid on top as props).
    if (h > water_level + 0.5f && up > 0.55f && inside_village(p.x, p.z, seed)) {
        const f32 cobble = noise::fbm2d(p.x * 0.7f, p.z * 0.7f, 1, 2.0f, 0.5f, seed + 808u);
        const f32 mud = noise::fbm2d(p.x * 0.22f, p.z * 0.22f, 2, 2.0f, 0.5f, seed + 909u);
        Vec3 town_ground = glm::mix(Vec3{0.30f, 0.23f, 0.15f}, Vec3{0.42f, 0.37f, 0.30f},
                                    glm::smoothstep(0.1f, 0.5f, cobble));
        town_ground = glm::mix(town_ground, Vec3{0.24f, 0.18f, 0.12f},
                               glm::smoothstep(0.1f, 0.45f, mud)); // wet muddy patches
        color = glm::mix(color, town_ground, glm::smoothstep(0.55f, 0.78f, up));
    }

    // Gentle height shading.
    return color * (0.85f + 0.15f * glm::clamp(h * 0.04f + 0.5f, 0.0f, 1.0f));
}

} // namespace alryn::worldgen
