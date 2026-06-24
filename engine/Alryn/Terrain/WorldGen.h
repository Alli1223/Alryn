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
// Hard ceiling on terrain elevation. `height()` is clamped to this so the surface can never rise
// above the band the streaming terrain meshes (StreamingTerrain y_max_) - otherwise a tall mountain
// would lift the (still-solid) density ground above the last meshed chunk and you'd walk up an
// invisible floor. Kept a couple of metres below y_max_ so the chunk's top samples stay clear air.
inline constexpr f32 max_terrain_height = 28.0f;

// --- Rivers ---------------------------------------------------------------
// Winding river network: the zero-contour of a low-frequency field, so rivers meander across the
// land. `river_amount` is ~1 in the channel and fades to 0 at the banks (the carve weight). Pure
// function of position (independent of height), so it's cheap to query anywhere - height() carves
// channels with it, roads cross them (and bridge them), and the map draws them as water.
inline f32 river_field(f32 x, f32 z, u32 seed) {
    return noise::fbm2d(x * 0.0035f, z * 0.0035f, 2, 2.0f, 0.5f, seed + 808u);
}
inline f32 river_amount(f32 x, f32 z, u32 seed) {
    return glm::smoothstep(0.024f, 0.0f, std::abs(river_field(x, z, seed)));
}
inline bool in_river(f32 x, f32 z, u32 seed) { return river_amount(x, z, seed) > 0.5f; }

// Smooth surface height at (x, z): blends ocean / lowland / big mountain ranges from a
// low-frequency "region" field, layers rolling hills + craggy ridge peaks + finer detail on land,
// then carves winding river channels - a varied landscape with mountains between the valleys.
inline f32 height(f32 x, f32 z, u32 seed) {
    const f32 region = noise::fbm2d(x * 0.006f, z * 0.006f, 3, 2.0f, 0.5f, seed + 101u);
    // Land-dominated (only the lowest region is ocean), so the continents are broad + connectable -
    // towns aren't stranded on little islands - with rivers + the odd sea for water variety.
    const f32 continental = glm::smoothstep(-0.55f, -0.05f, region);
    // Mountains are RARER than lowland (only the highest region values), so most of the land is
    // walkable valley that towns settle + roads link - with tall ranges rising between some of them.
    const f32 mountainous = glm::smoothstep(0.46f, 0.86f, region);

    const f32 base = glm::mix(-8.0f, 1.0f, continental) + mountainous * 11.0f; // taller ranges

    // Big rolling hills over all land (gentle, walkable, immersive undulation).
    const f32 hills = noise::fbm2d(x * 0.017f, z * 0.017f, 4, 2.0f, 0.5f, seed + 7u);
    // Mid-frequency terrain shape (bumps, dells) + fine surface roughness.
    const f32 detail = noise::fbm2d(x * 0.045f, z * 0.045f, 5, 2.0f, 0.5f, seed);
    const f32 fine = noise::fbm2d(x * 0.14f, z * 0.14f, 3, 2.0f, 0.5f, seed + 9u);
    // Ridged crags on mountainous ground -> craggy summits with lower saddles (passes) between them.
    const f32 ridge = 1.0f - std::abs(noise::fbm2d(x * 0.010f, z * 0.010f, 3, 2.0f, 0.5f, seed + 333u));

    const f32 land_amp = glm::mix(2.2f, 10.0f, mountainous) * continental + 0.4f;
    f32 h = base + continental * hills * 2.8f + mountainous * mountainous * ridge * 9.0f +
            land_amp * (detail * 0.7f + fine * 0.22f);

    // Carve winding river channels into the land (the water plane fills them; roads bridge them).
    const f32 river = river_amount(x, z, seed) * continental;
    h = glm::mix(h, std::min(h, water_level - 0.7f), river);
    // Cap the rare extreme summits so the surface always stays within the meshed band (no invisible
    // floor). A soft compression above most peaks keeps natural slopes rather than a flat plateau.
    if (h > 22.0f) {
        h = 22.0f + (h - 22.0f) * 0.4f; // 22..~26.5, well under max_terrain_height
    }
    return std::min(h, max_terrain_height);
}

inline f32 density(const Vec3& p, u32 seed) {
    return p.y - height(p.x, p.z, seed);
}

// Wetness field: drives lush forest (wet) vs open meadow / dry sand (dry). Biased positive so most
// land reads as green woodland, with drier clearings. LOW frequency so a biome tends to hold for a
// whole stretch between towns before it changes, rather than speckling.
inline f32 moisture(f32 x, f32 z, u32 seed) {
    return noise::fbm2d(x * 0.0075f, z * 0.0075f, 3, 2.0f, 0.5f, seed + 202u) + 0.14f;
}

// Temperature field: VERY large, low-frequency climate zones (so a desert / cold belt spans many
// towns, not a single patch), colder at altitude. ~0 cold .. 1 hot. With moisture + height this is
// what separates desert (hot+dry) from bog (wet lowland) from forest/plains/mountains.
inline f32 temperature(f32 x, f32 z, u32 seed) {
    const f32 base = noise::fbm2d(x * 0.0013f, z * 0.0013f, 3, 2.0f, 0.5f, seed + 511u);
    const f32 warm = glm::smoothstep(-0.5f, 0.5f, base);
    const f32 h = height(x, z, seed);
    return glm::clamp(warm - glm::smoothstep(3.0f, 12.0f, h) * 0.55f, 0.0f, 1.0f);
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

// --- Biomes ---------------------------------------------------------------
// A discrete classification of the land, derived from the shared height / slope / moisture /
// temperature fields. This is the linchpin everything downstream consumes: surface colouring (a
// smooth version below), which plants + trees + props scatter where, road routing difficulty, and
// the world map. Because it's a pure function of the noise fields it's deterministic + seamless and
// costs nothing to query anywhere (server, client, worker thread).
enum class Biome : u8 {
    Ocean,     // below the waterline
    Beach,     // gentle sand just above the water
    Desert,    // hot + dry: sand dunes
    Plains,    // open, drier grassland / meadow
    Forest,    // the default lush green woodland
    Bog,       // very wet lowland: dark murky swamp
    Mountains, // high or steep rocky ground
    Snow,      // the highest peaks
};

inline Biome biome_at(f32 x, f32 z, u32 seed) {
    const f32 h = height(x, z, seed);
    if (h < water_level + 0.25f) {
        return Biome::Ocean;
    }
    const f32 s = slope(x, z, seed);
    if (h > 11.0f) {
        return Biome::Snow;
    }
    if (h > 6.5f || s > 1.7f) {
        return Biome::Mountains;
    }
    if (h < water_level + 1.8f && s < 0.6f) {
        return Biome::Beach;
    }
    const f32 m = moisture(x, z, seed);
    if (m > 0.42f && h < water_level + 4.5f) {
        return Biome::Bog; // wet hollows turn to swamp
    }
    if (temperature(x, z, seed) > 0.58f && m < 0.08f) {
        return Biome::Desert; // hot + dry
    }
    return m > 0.02f ? Biome::Forest : Biome::Plains;
}

inline const char* biome_name(Biome b) {
    switch (b) {
        case Biome::Ocean: return "Ocean";
        case Biome::Beach: return "Coast";
        case Biome::Desert: return "Desert";
        case Biome::Plains: return "Plains";
        case Biome::Forest: return "Forest";
        case Biome::Bog: return "Bog";
        case Biome::Mountains: return "Mountains";
        case Biome::Snow: return "Snow";
    }
    return "?";
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
    // Settlement density varies across the world: a low-frequency field carves out WILDERNESS
    // bands where towns are far rarer, so some towns sit way out on their own (longer, lonelier
    // hauls) instead of every region being evenly dotted. `keep` is ~1 in settled land, ~0 in the
    // wild; the per-cell hash must fall under it to grow a town.
    const f32 settle = noise::fbm2d(static_cast<f32>(vcx) * 0.16f, static_cast<f32>(vcz) * 0.16f,
                                    2, 2.0f, 0.5f, seed + 777u);
    const f32 keep = glm::smoothstep(-0.40f, 0.18f, settle);
    if (detail::hash01(detail::tree_hash(vcx, vcz, salt)) > 0.62f * (0.45f + 0.55f * keep)) {
        return std::nullopt; // only some cells grow a town (sparser out in the wild)
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
    if (gh < water_level + 2.0f || gh > 9.0f) {
        return std::nullopt; // not in a buildable valley (above water, below the mountain slopes)
    }
    for (f32 ox : {-half, 0.0f, half}) {
        for (f32 oz : {-half, 0.0f, half}) {
            if (std::abs(height(cx + ox, cz + oz, seed) - gh) > 3.6f) {
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

    const Vec3 grass{0.27f, 0.52f, 0.21f};  // lush vibrant green
    const Vec3 grass2{0.40f, 0.64f, 0.26f}; // bright clearing green
    const Vec3 dirt{0.40f, 0.29f, 0.17f};   // warm leaf litter / bare soil
    const Vec3 sand{0.84f, 0.74f, 0.46f};
    const Vec3 wet_sand{0.46f, 0.42f, 0.32f};
    const Vec3 rock{0.44f, 0.44f, 0.48f};
    const Vec3 snow{0.93f, 0.94f, 0.98f};

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

    // Desert: hot + dry, gentle, above the beach band -> warm rippled sand dunes. (Smooth masks
    // matching biome_at's thresholds, so the look agrees with the classification without seams.)
    const f32 t = temperature(p.x, p.z, seed);
    const f32 desert_mask = glm::smoothstep(0.50f, 0.62f, t) * glm::smoothstep(0.16f, 0.0f, m) *
                            glm::smoothstep(0.55f, 0.78f, up) *
                            glm::smoothstep(water_level + 1.5f, water_level + 3.5f, h) *
                            (1.0f - snow_amt);
    if (desert_mask > 0.001f) {
        const Vec3 dune{0.82f, 0.70f, 0.42f};
        const Vec3 dune2{0.90f, 0.80f, 0.54f};
        const f32 ripple = noise::fbm2d(p.x * 0.09f, p.z * 0.09f, 2, 2.0f, 0.5f, seed + 606u);
        const Vec3 desert_col = glm::mix(dune, dune2, glm::smoothstep(-0.2f, 0.3f, ripple));
        color = glm::mix(color, desert_col, desert_mask);
    }
    // Bog: very wet, low-lying, gentle -> dark murky muck with near-black water-logged hollows.
    const f32 bog_mask = glm::smoothstep(0.34f, 0.46f, m) *
                         glm::smoothstep(water_level + 5.0f, water_level + 1.0f, h) *
                         glm::smoothstep(0.55f, 0.8f, up);
    if (bog_mask > 0.001f) {
        const Vec3 muck{0.22f, 0.26f, 0.16f};
        const Vec3 muck2{0.13f, 0.16f, 0.12f};
        const f32 puddle = noise::fbm2d(p.x * 0.08f, p.z * 0.08f, 2, 2.0f, 0.5f, seed + 707u);
        const Vec3 bog_col = glm::mix(muck, muck2, glm::smoothstep(0.0f, 0.3f, puddle));
        color = glm::mix(color, bog_col, bog_mask * 0.85f);
    }

    // (The worn dirt road colour is overlaid separately via roads::tint_surface while
    // meshing, so this base colour stays independent of the road network.)

    // Town ground: GRASSY open areas (so the town has green, not all mud) with worn bare-earth
    // patches trampled through it. The dirt streets + light flagstones are overlaid on top as the
    // road network (town_path_tint + Path props), so the green sits between the paths.
    if (h > water_level + 0.5f && up > 0.55f && inside_village(p.x, p.z, seed)) {
        const f32 worn = noise::fbm2d(p.x * 0.13f, p.z * 0.13f, 2, 2.0f, 0.5f, seed + 909u);
        const Vec3 town_grass{0.30f, 0.5f, 0.22f}; // lush green over most of the open ground
        const Vec3 town_dirt{0.47f, 0.36f, 0.23f}; // warm bare earth only on the most-trodden spots
        Vec3 town_ground = glm::mix(town_grass, town_dirt, glm::smoothstep(0.62f, 0.95f, worn));
        color = glm::mix(color, town_ground, glm::smoothstep(0.55f, 0.78f, up));
    }

    // Gentle height shading.
    return color * (0.85f + 0.15f * glm::clamp(h * 0.04f + 0.5f, 0.0f, 1.0f));
}

} // namespace alryn::worldgen
