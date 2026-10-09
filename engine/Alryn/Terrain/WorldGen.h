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
// invisible floor. Set WELL ABOVE the natural summit (~42 m) so it's just a safety net and never
// actually flattens a peak; kept below y_max_ so the chunk's top samples stay clear air. The
// column-cached field fill makes the tall band this requires cheap (one height() per column).
inline constexpr f32 max_terrain_height = 48.0f;

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

// --- Snow -----------------------------------------------------------------
// Snow lies on everything above the snowline: the peaks and the high alpine plateaus. 0 below it ..
// 1 fully snowbound. Shared by the surface colour, the biome classifier, the towns that settle up
// there (Village::snowy) and the client's snowfall, so they all agree on where winter is.
inline constexpr f32 snowline_lo = 8.5f;  // the first dusting
inline constexpr f32 snowline_hi = 12.5f; // deep snow from here up
inline f32 snow_cover(f32 h) { return glm::smoothstep(snowline_lo, snowline_hi, h); }

// --- Alpine plateaus ------------------------------------------------------
// Some mountain massifs hold a broad SHELF of gentle upland - a snowbound plateau ringed by crags,
// climbed to over a long escarpment - where hardy towns settle above the snowline. 0 off the shelf ..
// 1 on it. `region` is base_height's continental/mountain field (so the shelves sit in the high
// country, not out on the coastal lowlands).
inline f32 plateau_amount(f32 x, f32 z, u32 seed, f32 region) {
    const f32 massif = glm::smoothstep(0.16f, 0.38f, region);
    if (massif <= 0.0f) {
        return 0.0f;
    }
    const f32 shelf = noise::fbm2d(x * 0.0036f, z * 0.0036f, 2, 2.0f, 0.5f, seed + 919u);
    return massif * glm::smoothstep(-0.12f, 0.12f, shelf);
}

// The NATURAL surface height at (x, z): blends ocean / lowland / big mountain ranges from a
// low-frequency "region" field, layers rolling hills + craggy ridge peaks + finer detail on land,
// lifts the odd massif into a snowbound alpine PLATEAU, then carves winding river channels - a varied
// landscape with mountains between the valleys. This is the land BEFORE anything is built on it: the
// town layout (village sites, streets, house plots) and the road network are planned from it.
// Everything that sits on / walks the ground uses height() below instead, which levels this under each
// town building.
inline f32 base_height(f32 x, f32 z, u32 seed) {
    const f32 region = noise::fbm2d(x * 0.006f, z * 0.006f, 3, 2.0f, 0.5f, seed + 101u);
    // Land-dominated (only the lowest region is ocean), so the continents are broad + connectable -
    // towns aren't stranded on little islands - with rivers + the odd sea for water variety.
    const f32 continental = glm::smoothstep(-0.55f, -0.05f, region);
    // Mountains are RARER than lowland (only the highest region values), so most of the land is
    // walkable valley that towns settle + roads link - with tall ranges rising between some of them.
    const f32 mountainous = glm::smoothstep(0.46f, 0.86f, region);

    const f32 base = glm::mix(-8.0f, 1.0f, continental) + mountainous * 16.0f; // tall, dramatic ranges

    // Big rolling hills over all land (gentle, walkable, immersive undulation).
    const f32 hills = noise::fbm2d(x * 0.017f, z * 0.017f, 4, 2.0f, 0.5f, seed + 7u);
    // Mid-frequency terrain shape (bumps, dells) + fine surface roughness.
    const f32 detail = noise::fbm2d(x * 0.045f, z * 0.045f, 5, 2.0f, 0.5f, seed);
    const f32 fine = noise::fbm2d(x * 0.14f, z * 0.14f, 3, 2.0f, 0.5f, seed + 9u);
    // Ridged crags on mountainous ground -> craggy summits with lower saddles (passes) between them.
    const f32 ridge = 1.0f - std::abs(noise::fbm2d(x * 0.010f, z * 0.010f, 3, 2.0f, 0.5f, seed + 333u));

    const f32 land_amp = glm::mix(2.2f, 10.0f, mountainous) * continental + 0.4f;
    f32 h = base + continental * hills * 2.8f + mountainous * mountainous * ridge * 13.0f +
            land_amp * (detail * 0.7f + fine * 0.22f);

    // An alpine plateau: the massif's craggy relief eases into a broad, gently rolling shelf high above
    // the valleys (above the snowline), its level drifting slowly so neighbouring shelves differ.
    if (const f32 shelf = plateau_amount(x, z, seed, region); shelf > 0.0f) {
        const f32 level = 14.0f + 2.5f * noise::fbm2d(x * 0.0025f, z * 0.0025f, 2, 2.0f, 0.5f, seed + 929u);
        const f32 upland = level + hills * 1.5f + detail * 0.8f + fine * 0.22f;
        h = glm::mix(h, upland, glm::smoothstep(0.0f, 1.0f, shelf) * continental);
    }

    // Carve winding river channels into the land (the water plane fills them; roads bridge them).
    const f32 river = river_amount(x, z, seed) * continental;
    h = glm::mix(h, std::min(h, water_level - 0.7f), river);
    // Only a hard safety clamp (well above the natural summit), so the surface can never exceed the
    // meshed band - there's no soft compression, so mountains keep their full dramatic height.
    return std::min(h, max_terrain_height);
}

// The GROUND height at (x, z): base_height() with a flat building pad levelled under every town
// house (its footprint + yard, at the natural height of its centre), easing back to the natural
// land over a short skirt - so floors, doorsteps and walls sit on flat ground instead of a slope
// poking up through them. This is the surface the terrain is meshed from and that players, NPCs,
// wagons and props stand on. Out of line (Terrain/WorldGen.cpp): it needs the town layout.
f32 height(f32 x, f32 z, u32 seed);

// True inside the walls of a town building at (x, z) (grown by `margin`) - indoors, where no
// grass or flowers grow.
bool under_building(f32 x, f32 z, u32 seed, f32 margin = 0.0f);

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
// Overload for callers that already sampled the height at (x,z) - the altitude chill reuses it
// instead of paying for another height() evaluation (the world-map raster grids its heights).
inline f32 temperature(f32 x, f32 z, u32 seed, f32 h) {
    const f32 base = noise::fbm2d(x * 0.0013f, z * 0.0013f, 3, 2.0f, 0.5f, seed + 511u);
    const f32 warm = glm::smoothstep(-0.5f, 0.5f, base);
    return glm::clamp(warm - glm::smoothstep(3.0f, 12.0f, h) * 0.55f, 0.0f, 1.0f);
}
inline f32 temperature(f32 x, f32 z, u32 seed) {
    return temperature(x, z, seed, base_height(x, z, seed));
}

// Roads connecting nearby towns (routed to avoid water) live in Terrain/RoadNetwork.h.
// The old noise-contour "paths" are gone; road colouring is overlaid while meshing
// (roads::tint_surface) so this header stays free of the road-network dependency.

// Local terrain slope at (x,z) (how much it tilts over ~1 unit) of the natural land. Shared by
// the scatters and the road-flatness gate.
inline f32 slope(f32 x, f32 z, u32 seed) {
    const f32 g = base_height(x, z, seed);
    return std::abs(base_height(x + 1.0f, z, seed) - g) +
           std::abs(base_height(x, z + 1.0f, seed) - g);
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

// The classification itself, over already-sampled field values (h = height, s = slope as
// slope() defines it, m = moisture, t = temperature). Callers that grid-sample the fields
// (the world-map raster) classify without re-evaluating them; biome_at below samples then
// delegates, so the thresholds live in exactly one place.
inline Biome classify_biome(f32 h, f32 s, f32 m, f32 t) {
    if (h < water_level + 0.25f) {
        return Biome::Ocean;
    }
    if (h > 11.0f) {
        return Biome::Snow;
    }
    if (h > 6.5f || s > 1.7f) {
        return Biome::Mountains;
    }
    if (h < water_level + 1.8f && s < 0.6f) {
        return Biome::Beach;
    }
    if (m > 0.42f && h < water_level + 4.5f) {
        return Biome::Bog; // wet hollows turn to swamp
    }
    if (t > 0.58f && m < 0.08f) {
        return Biome::Desert; // hot + dry
    }
    return m > 0.02f ? Biome::Forest : Biome::Plains;
}

inline Biome biome_at(f32 x, f32 z, u32 seed) {
    const f32 h = base_height(x, z, seed);
    if (h < water_level + 0.25f) {
        return Biome::Ocean; // cheap early-out before the slope/moisture/temperature samples
    }
    return classify_biome(h, slope(x, z, seed), moisture(x, z, seed),
                          temperature(x, z, seed, h));
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
// Medieval settlements sit on flat, above-water ground, sparsely scattered on a coarse grid - long
// lonely roads between them. They come in four TIERS, from a hamlet of a few cottages round a well to
// a walled city of hundreds of homes, and each lays its streets out in its own STYLE. The placement
// lives here (it only needs the height field + hash) so terrain colouring and the scatters can ask
// "am I in a village?"; the actual buildings are laid out in World/Village.h.
inline constexpr f32 village_cell = 250.0f;     // grid spacing of candidate towns
inline constexpr f32 village_half_max = 100.0f;  // a great city's half-width (the largest settlement)
// A settlement this high up sits above the snowline: snow on its roofs + streets, braziers by night.
inline constexpr f32 snow_town_ground = 10.5f;

enum class TownTier : u8 {
    Hamlet = 0,  // a handful of cottages round a well, no wall
    Village = 1, // a lane of homes + a small market
    Town = 2,    // the walled market town
    City = 3,    // a great walled city: ring after ring of streets, hundreds of homes, a keep
};
inline const char* town_tier_name(TownTier t) {
    switch (t) {
        case TownTier::Hamlet: return "HAMLET";
        case TownTier::Village: return "VILLAGE";
        case TownTier::Town: return "TOWN";
        case TownTier::City: return "CITY";
    }
    return "TOWN";
}

// How a settlement's streets are laid out (World/Village.h town_streets).
enum class TownLayout : u8 {
    Lane = 0,   // one high street through the middle + a few short lanes off it
    Radial = 1, // a ring road round the market + avenues out to the gates + spokes
    Grid = 2,   // a planned grid of streets (turned to the town's own heading)
    Rings = 3,  // a city: concentric ring roads crossed by radial avenues
};

struct Village {
    Vec2 center{0.0f}; // xz of the town centre
    f32 ground = 0.0f; // ground height at the centre
    f32 half = 38.0f;  // the settlement's half-width
    u32 vseed = 0;     // per-village layout seed
    TownTier tier = TownTier::Town;
    TownLayout layout = TownLayout::Radial;
    bool snowy = false; // above the snowline (snowbound roofs + streets, braziers)
};

// Does this settlement have a market square (a hamlet only has a well on its green)? / Is it walled?
inline bool has_market(const Village& v) { return v.tier != TownTier::Hamlet; }
inline bool has_wall(const Village& v) { return v.tier != TownTier::Hamlet; }

// The settlement whose origin falls in coarse cell (vcx,vcz), if the ground suits it (and no great
// city next door has claimed the land). Deterministic + cached (Terrain/WorldGen.cpp) - it's asked
// per terrain vertex, and a cell's answer depends on its neighbours (a city keeps them clear).
std::optional<Village> village_at(int vcx, int vcz, u32 seed);

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
    // Kept COOL + slightly dark on purpose: the renderer's warm split-tone grade + golden sun
    // push anything bright toward cream (a near-white albedo lands looking like pale sand).
    // Sitting the albedo below the blow-out range with a strong blue lean is what actually
    // reads as cold snow on screen.
    const Vec3 snow{0.58f, 0.68f, 0.88f};

    // Forest floor: blend two greens by a mid-frequency field, with patches of
    // bare earth / leaf-litter, so the ground reads varied rather than flat green.
    const f32 tone = noise::fbm2d(p.x * 0.06f, p.z * 0.06f, 2, 2.0f, 0.5f, seed + 303u);
    const f32 litter = noise::fbm2d(p.x * 0.11f, p.z * 0.11f, 2, 2.0f, 0.5f, seed + 404u);
    Vec3 ground = glm::mix(grass, grass2, glm::smoothstep(-0.25f, 0.25f, tone));
    ground = glm::mix(ground, dirt, glm::smoothstep(0.22f, 0.5f, litter));
    // Flat ground: desert sand <-> forest floor by moisture.
    Vec3 flat = glm::mix(sand, ground, glm::smoothstep(-0.15f, 0.2f, m));
    // Mountainsides: above the foothills, dry ground turns to grey scree instead of smooth dune
    // sand (whole peaks used to read as cream sand right up to the snowline).
    const Vec3 scree{0.52f, 0.50f, 0.47f};
    flat = glm::mix(flat, scree, glm::smoothstep(5.0f, 8.5f, h));
    // Steep slopes turn rocky.
    Vec3 color = glm::mix(rock, flat, glm::smoothstep(0.5f, 0.75f, up));

    // Sandy beach band just above the water on gentle ground.
    const f32 beach = glm::smoothstep(water_level + 2.2f, water_level + 0.3f, h) *
                      glm::smoothstep(0.6f, 0.85f, up);
    color = glm::mix(color, sand, beach);
    // Darker silt below the waterline.
    color = glm::mix(color, wet_sand, glm::smoothstep(water_level + 0.2f, water_level - 2.5f, h));
    // Snow up high: it clings to everything but the sheer cliff faces (a gentle-ground-only
    // gate left whole peaks reading as cream rock/sand), and what rock still shows through
    // cools toward blue-grey granite so the summits read cold.
    const f32 alt = snow_cover(h);
    const f32 snow_amt = alt * glm::smoothstep(0.30f, 0.58f, up);
    color = glm::mix(color, Vec3{0.47f, 0.50f, 0.58f}, alt * (1.0f - snow_amt) * 0.8f); // exposed granite
    color = glm::mix(color, snow, snow_amt);

    // Desert: hot + dry, gentle, above the beach band -> warm rippled sand dunes. (Smooth masks
    // matching biome_at's thresholds, so the look agrees with the classification without seams.)
    const f32 t = temperature(p.x, p.z, seed);
    const f32 desert_mask = glm::smoothstep(0.50f, 0.62f, t) * glm::smoothstep(0.16f, 0.0f, m) *
                            glm::smoothstep(0.55f, 0.78f, up) *
                            glm::smoothstep(water_level + 1.5f, water_level + 3.5f, h) *
                            glm::smoothstep(9.0f, 6.0f, h) * // dunes stay in the lowlands (scree above)
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
    if (h > water_level + 0.5f && up > 0.55f) {
        if (const auto v = village_containing(p.x, p.z, seed, 7.0f)) {
            // Grazed commons: full green inside the walls, easing out over a soft verge that
            // reaches a few metres beyond them - the binary inside-test used to snap storybook
            // grass to raw biome ground in a single vertex (a sawtooth seam at the outline).
            const Vec2 d{p.x - v->center.x, p.z - v->center.y};
            const f32 r = town_radius(*v, std::atan2(d.y, d.x), seed);
            const f32 verge = glm::smoothstep(r + 7.0f, r - 4.0f, glm::length(d));
            if (verge > 0.001f) {
                const f32 worn = noise::fbm2d(p.x * 0.13f, p.z * 0.13f, 2, 2.0f, 0.5f, seed + 909u);
                Vec3 town_ground;
                if (v->snowy) {
                    // A snowbound town: drifts over the commons, trodden down to grey slush where the
                    // townsfolk walk, with the odd patch of frozen earth showing through.
                    const Vec3 drift{0.60f, 0.69f, 0.88f};
                    const Vec3 drift2{0.66f, 0.73f, 0.90f};
                    const Vec3 slush{0.46f, 0.48f, 0.55f};
                    town_ground = glm::mix(drift, drift2, glm::smoothstep(-0.2f, 0.45f, worn));
                    town_ground = glm::mix(town_ground, slush, glm::smoothstep(0.55f, 0.95f, worn) * 0.7f);
                } else {
                    const Vec3 town_grass{0.31f, 0.56f, 0.22f}; // bright storybook green over most of the open ground
                    const Vec3 town_grass2{0.40f, 0.63f, 0.26f}; // sunnier clearing green (variation, not mud)
                    const Vec3 town_dirt{0.47f, 0.36f, 0.23f};  // warm bare earth only on the most-trodden spots
                    town_ground = glm::mix(town_grass, town_grass2, glm::smoothstep(-0.2f, 0.45f, worn));
                    town_ground = glm::mix(town_ground, town_dirt, glm::smoothstep(0.74f, 1.05f, worn));
                }
                color = glm::mix(color, town_ground, glm::smoothstep(0.55f, 0.78f, up) * verge);
            }
        }
    }

    // Gentle height shading.
    return color * (0.85f + 0.15f * glm::clamp(h * 0.04f + 0.5f, 0.0f, 1.0f));
}

} // namespace alryn::worldgen
