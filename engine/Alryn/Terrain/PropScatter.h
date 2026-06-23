#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Noise.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/ScatterHash.h> // detail::tree_hash / hash01
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/World/Prop.h>
#include <Alryn/World/Village.h>

#include <cmath>
#include <vector>

namespace alryn {

namespace detail {

// Local terrain slope at (x,z): how much the ground tilts over ~1 unit.
inline f32 ground_slope(f32 x, f32 z, u32 seed) {
    const f32 g = worldgen::height(x, z, seed);
    return std::abs(worldgen::height(x + 1.0f, z, seed) - g) +
           std::abs(worldgen::height(x, z + 1.0f, seed) - g);
}

} // namespace detail

// Deterministically places discrete props (bushes, rocks, rare houses) over the
// chunk. Bushes like gentle green ground, rocks tolerate slopes, houses need a
// flat, dry-ish, above-water clearing. Cell grids are global so chunks tile.
inline std::vector<PropInstance> scatter_props(int cx, int cz, f32 chunk_world, u32 seed) {
    std::vector<PropInstance> out;
    const f32 x0 = static_cast<f32>(cx) * chunk_world;
    const f32 z0 = static_cast<f32>(cz) * chunk_world;

    auto place = [&](f32 cell, u32 salt, PropCategory cat, int variants, auto accept) {
        // Scan every global grid cell whose (jittered) site could land in this chunk, and emit
        // it only if its site is actually inside the chunk bounds - so each site is owned by
        // exactly ONE chunk. This matters when the cell is larger than a chunk (e.g. the 23 m
        // lantern grid over 8 m chunks): the old code let every chunk that floored to the same
        // grid cell emit the SAME site, stacking 3x3 identical lanterns (over-bright). It also
        // removes duplicate/missing props at chunk seams for the finer grids.
        const int gx0 = static_cast<int>(std::floor(x0 / cell)) - 1;
        const int gz0 = static_cast<int>(std::floor(z0 / cell)) - 1;
        const int gx1 = static_cast<int>(std::floor((x0 + chunk_world) / cell)) + 1;
        const int gz1 = static_cast<int>(std::floor((z0 + chunk_world) / cell)) + 1;
        for (int gz = gz0; gz <= gz1; ++gz) {
            for (int gx = gx0; gx <= gx1; ++gx) {
                const f32 jx = (detail::hash01(detail::tree_hash(gx, gz, salt + 1u)) - 0.5f) * cell * 0.7f;
                const f32 jz = (detail::hash01(detail::tree_hash(gx, gz, salt + 2u)) - 0.5f) * cell * 0.7f;
                const f32 wx = (static_cast<f32>(gx) + 0.5f) * cell + jx;
                const f32 wz = (static_cast<f32>(gz) + 0.5f) * cell + jz;
                if (wx < x0 || wx >= x0 + chunk_world || wz < z0 || wz >= z0 + chunk_world) {
                    continue; // this site belongs to a neighbouring chunk
                }
                const f32 gh = worldgen::height(wx, wz, seed);
                const u32 h = detail::tree_hash(gx, gz, salt + 3u);
                if (!accept(wx, wz, gh, h)) {
                    continue;
                }
                PropInstance p;
                p.category = cat;
                p.variant = static_cast<u8>(detail::tree_hash(gx, gz, salt + 4u) % static_cast<u32>(variants));
                p.position = Vec3{wx, gh, wz};
                p.yaw = detail::hash01(detail::tree_hash(gx, gz, salt + 5u)) * TwoPi;
                p.scale = 1.0f;
                out.push_back(p);
            }
        }
    };

    // Keep loose forest props off the roads and out of the towns.
    auto off_path = [&](f32 x, f32 z) {
        return roads::distance(x, z, seed) > roads::road_half_width + 0.8f &&
               !worldgen::inside_village(x, z, seed, 2.0f);
    };

    // Bushes: dense forest undergrowth on gentle, above-water, non-desert ground.
    place(2.2f, seed + 6100u, PropCategory::Bush, 3, [&](f32 x, f32 z, f32 gh, u32 h) {
        if (gh < worldgen::water_level + 0.8f || gh > 9.0f) return false;
        const f32 moist = worldgen::moisture(x, z, seed);
        if (moist < -0.05f) return false;
        if (detail::ground_slope(x, z, seed) > 2.2f || !off_path(x, z)) return false;
        return detail::hash01(h) < 0.30f + glm::clamp(moist, 0.0f, 0.5f) * 0.5f; // lusher when wet
    });

    // Rocks / mossy boulders: tolerate slopes, above water.
    place(5.0f, seed + 6200u, PropCategory::Rock, 3, [&](f32 x, f32 z, f32 gh, u32 h) {
        if (gh < worldgen::water_level + 0.4f || !off_path(x, z)) return false;
        return detail::hash01(h) < 0.32f;
    });

    // Fallen logs: occasional forest-floor debris (you bump into them) on flattish,
    // moist, wooded ground.
    place(5.5f, seed + 6400u, PropCategory::Log, 3, [&](f32 x, f32 z, f32 gh, u32 h) {
        if (gh < worldgen::water_level + 1.0f || !off_path(x, z)) return false;
        if (worldgen::moisture(x, z, seed) < 0.0f) return false;
        if (detail::ground_slope(x, z, seed) > 1.6f) return false;
        return detail::hash01(h) < 0.18f;
    });

    // Standalone lanterns dotted across the world - glowing waypoints in the dark
    // forest that pool warm light on the ground and cast shadows at night. A coarse
    // grid on fairly flat, dry ground, kept off the paths (which have their own) and
    // out of the towns.
    place(23.0f, seed + 6800u, PropCategory::Lantern, 1, [&](f32 x, f32 z, f32 gh, u32 h) {
        if (gh < worldgen::water_level + 0.9f) return false;
        if (detail::ground_slope(x, z, seed) > 1.3f) return false;
        if (!off_path(x, z)) return false;
        return detail::hash01(h) < 0.5f;
    });

    // Glowing magic crystal clusters: an occasional, magical detail dotted across the wilds (off
    // roads + out of towns), favouring rockier ground. They glow + pool coloured light at night.
    place(17.0f, seed + 6900u, PropCategory::Crystal, kCrystalVariants, [&](f32 x, f32 z, f32 gh, u32 h) {
        if (gh < worldgen::water_level + 0.7f || !off_path(x, z)) return false;
        if (detail::ground_slope(x, z, seed) > 3.2f) return false;
        return detail::hash01(h) < 0.16f; // rare - a sprinkle, not a field
    });

    // Bioluminescent mushroom clusters: damp, shady forest floor (needs moisture), off paths/towns -
    // they glow softly at night.
    place(9.5f, seed + 6950u, PropCategory::GlowShroom, kGlowShroomVariants, [&](f32 x, f32 z, f32 gh, u32 h) {
        if (gh < worldgen::water_level + 0.8f || gh > 9.0f || !off_path(x, z)) return false;
        if (worldgen::moisture(x, z, seed) < 0.12f) return false; // damp ground only
        if (detail::ground_slope(x, z, seed) > 2.0f) return false;
        return detail::hash01(h) < 0.14f;
    });

    // Campfires: a rare cosy rest-spot in a flat dry clearing (off paths/towns) that lights the
    // night - the wilderness-camp vibe from the references.
    place(40.0f, seed + 6980u, PropCategory::Campfire, 1, [&](f32 x, f32 z, f32 gh, u32 h) {
        if (gh < worldgen::water_level + 1.0f || !off_path(x, z)) return false;
        if (detail::ground_slope(x, z, seed) > 0.9f) return false; // a flat clearing
        return detail::hash01(h) < 0.3f;
    });

    // Stone monuments / ruins: occasional ancient landmarks on flattish, above-water ground.
    place(34.0f, seed + 7000u, PropCategory::Monument, kMonumentVariants, [&](f32 x, f32 z, f32 gh, u32 h) {
        if (gh < worldgen::water_level + 1.0f || !off_path(x, z)) return false;
        if (detail::ground_slope(x, z, seed) > 1.4f) return false;
        return detail::hash01(h) < 0.4f;
    });

    // Wooden watchtowers: a rare lookout on flat, dry, open ground well clear of towns.
    place(70.0f, seed + 7050u, PropCategory::Watchtower, 1, [&](f32 x, f32 z, f32 gh, u32 h) {
        if (gh < worldgen::water_level + 1.2f) return false;
        if (worldgen::inside_village(x, z, seed, 18.0f)) return false;
        if (detail::ground_slope(x, z, seed) > 0.8f || !off_path(x, z)) return false;
        return detail::hash01(h) < 0.35f;
    });

    // Roadside fences as a proper post-and-rail run: march POSTS along each road edge at a
    // varying spacing and join each kept post to the previous with a RAIL stretched to that
    // exact gap. The run BREAKS (no rail bridges it) wherever a post is dropped, so fences
    // never block a crossing road or a town gate, and intermittent noise leaves some
    // stretches open instead of fencing every road end to end. Posts/rails are placed
    // deterministically per road segment (independent of the chunk) and each piece is owned
    // by the chunk containing it, so chunks tile seamlessly without doubling.
    {
        constexpr f32 edge_off = roads::road_half_width + 0.45f; // just outside the road
        constexpr f32 base_gap = 2.5f;                           // nominal post spacing
        constexpr f32 max_rail = 4.2f;                           // never bridge a wider gap
        const u32 salt = seed + 6700u;
        const Vec2 cc{x0 + chunk_world * 0.5f, z0 + chunk_world * 0.5f};
        const std::vector<roads::Segment> segs = roads::gather(cc, chunk_world + base_gap, seed);

        const f32 cx_lo = x0;
        const f32 cx_hi = x0 + chunk_world;
        const f32 cz_lo = z0;
        const f32 cz_hi = z0 + chunk_world;
        auto in_chunk = [&](const Vec2& p) {
            return p.x >= cx_lo && p.x < cx_hi && p.y >= cz_lo && p.y < cz_hi;
        };
        // A point may host a roadside fence iff it's above water, well clear of any town,
        // and NOT sitting on another (crossing) road - so we never wall off a junction.
        auto fence_ok = [&](const Vec2& p) {
            if (worldgen::height(p.x, p.y, seed) < worldgen::water_level + 0.6f) return false;
            if (worldgen::inside_village(p.x, p.y, seed, 8.0f)) return false;
            if (roads::distance(p.x, p.y, seed) < roads::road_half_width + 0.25f) return false;
            return true;
        };

        for (const roads::Segment& s : segs) {
            const Vec2 d = s.b - s.a;
            const f32 L = glm::length(d);
            if (L < base_gap) continue;
            const Vec2 dir = d / L;
            const Vec2 perp{-dir.y, dir.x};
            const u32 seg_h = detail::tree_hash(static_cast<int>(std::lround(s.a.x + s.b.x)),
                                                static_cast<int>(std::lround(s.a.y + s.b.y)), salt);

            for (f32 side : {-1.0f, 1.0f}) {
                bool have_prev = false;
                Vec2 prev_pos{0.0f};
                f32 t = base_gap * 0.5f;
                int idx = 0;
                while (t <= L) {
                    const Vec2 pos = s.a + dir * t + perp * (side * edge_off);
                    // Intermittent: only fence stretches where a low-frequency band is high,
                    // so fences come and go along the network rather than lining everything.
                    const f32 band = noise::fbm2d(pos.x * 0.016f, pos.y * 0.016f, 2, 2.0f, 0.5f,
                                                  salt + (side < 0.0f ? 31u : 67u));
                    const bool ok = band > 0.08f && fence_ok(pos);
                    if (ok) {
                        const f32 gh = worldgen::height(pos.x, pos.y, seed);
                        if (in_chunk(pos)) {
                            PropInstance post;
                            post.position = Vec3{pos.x, gh, pos.y};
                            post.yaw = std::atan2(-dir.y, dir.x);
                            const u32 ph = detail::tree_hash(idx, static_cast<int>(side * 3.0f), seg_h);
                            if (detail::hash01(ph) < 0.05f) {
                                post.category = PropCategory::Lantern;
                                post.variant = 0;
                            } else {
                                post.category = PropCategory::Fence;
                                post.variant = static_cast<u8>(seg_h % 2u);
                            }
                            out.push_back(post);
                        }
                        // Join to the previous kept post with a rail stretched to the gap.
                        if (have_prev) {
                            const Vec2 mid = (prev_pos + pos) * 0.5f;
                            const f32 gap = glm::length(pos - prev_pos);
                            if (gap > 0.4f && gap < max_rail && in_chunk(mid)) {
                                const Vec2 rd = (pos - prev_pos) / gap;
                                PropInstance rail;
                                rail.category = PropCategory::FenceRail;
                                rail.variant = static_cast<u8>(seg_h % 2u);
                                rail.position = Vec3{mid.x, worldgen::height(mid.x, mid.y, seed), mid.y};
                                rail.yaw = std::atan2(-rd.y, rd.x);
                                rail.length = gap; // stretch the unit rail to bridge the gap
                                out.push_back(rail);
                            }
                        }
                        prev_pos = pos;
                        have_prev = true;
                    } else {
                        have_prev = false; // break the run (junction / town / open stretch)
                    }
                    const f32 jitter =
                        0.78f + detail::hash01(detail::tree_hash(idx, 9, seg_h)) * 0.9f; // 0.78..1.68
                    t += base_gap * jitter;
                    ++idx;
                }
            }
        }
    }

    // Village structures (cottages, walls, gate towers) whose pieces fall in this
    // chunk. Towns are large but sparse, so checking the 3x3 village cells around
    // the chunk and filtering to its bounds is cheap (and the chunk result caches).
    {
        const int vcx = static_cast<int>(std::floor((x0 + chunk_world * 0.5f) / worldgen::village_cell));
        const int vcz = static_cast<int>(std::floor((z0 + chunk_world * 0.5f) / worldgen::village_cell));
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto village = worldgen::village_at(vcx + dx, vcz + dz, seed);
                if (!village) {
                    continue;
                }
                for (const PropInstance& p : village_props(*village, seed)) {
                    if (p.position.x >= x0 && p.position.x < x0 + chunk_world &&
                        p.position.z >= z0 && p.position.z < z0 + chunk_world) {
                        out.push_back(p);
                    }
                }
            }
        }
    }

    return out;
}

} // namespace alryn
