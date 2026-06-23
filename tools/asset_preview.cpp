// asset_preview - render a single generated asset (house, tree, prop, ...) in isolation, on a
// small lit ground plane, to a PPM image. This is the iteration harness for art work: tweak a
// procedural generator, re-render the one asset, and compare it against a reference image.
//
//   asset_preview <category> [variant] [out.ppm]
//     <category>   house | townhouse | pub | blacksmith | tree | bush | rock | log | fence | rail |
//                  lantern | well | gate | tower | wall | market | fountain | planter | bridge |
//                  stonebridge | path | river | decor | wagon | wheel | fern | mushroom
//     [variant]    which variant to render. Omitted -> render EVERY variant to its own file.
//     [out.ppm]    output path (only with an explicit variant; default asset_<cat>_<variant>.ppm)
//
// Reuses the headless OffscreenRenderer (the same mesh.* shaders + lighting the game uses), so the
// preview matches in-game. Writes next to the binary (build/bin); `make asset` converts to PNG.

#include "support/OffscreenRenderer.h"

#include <Alryn/Core/Log.h>
#include <Alryn/Renderer/MeshPrimitives.h>
#include <Alryn/World/PropLibrary.h>
#include <Alryn/World/VehicleTypes.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace alryn;

namespace {

// A renderable asset: a set of mesh parts (each with a tint) gathered from a generator.
struct AssetPart {
    MeshData mesh;
    Vec4 tint{1.0f};
};
struct Asset {
    std::vector<AssetPart> parts;
};

// Tint for a prop layer when previewing through the lit mesh shader: lights/glow read brighter so
// they don't look like dull grey boxes; everything else renders as authored.
Vec4 layer_tint(PropLayer layer) {
    switch (layer) {
        case PropLayer::Emissive:
        case PropLayer::Glow: return Vec4{1.7f, 1.55f, 1.2f, 1.0f};
        default: return Vec4{1.0f};
    }
}

void add_prop(Asset& a, const PropDef& def) {
    for (const PropPart& p : def.parts) {
        if (p.mesh.indices.empty()) {
            continue;
        }
        a.parts.push_back({p.mesh, layer_tint(p.layer)});
    }
}

// The foliage tint a scattered tree of this variant gets in-game (so the preview matches): the
// baked deep-green canopy is re-coloured by target/leaf_base (conifers green, deciduous autumn).
Vec3 tree_foliage_tint(int variant) {
    const Vec3 leaf_base{0.16f, 0.40f, 0.19f};
    if (variant == 0 || variant == 4) {
        return Vec3{0.9f, 1.0f, 0.86f}; // pine / dead: stay green
    }
    static const Vec3 autumn[] = {{0.34f, 0.52f, 0.22f}, {0.84f, 0.66f, 0.22f},
                                  {0.88f, 0.46f, 0.16f}, {0.74f, 0.30f, 0.16f}};
    return autumn[static_cast<usize>(variant) % 4] / leaf_base;
}

// How many variants a category has (for the "render all" mode).
int variant_count(const std::string& cat) {
    if (cat == "house") return 8;
    if (cat == "tree") return 5;
    if (cat == "decor") return 8;
    if (cat == "crystal") return 4;
    if (cat == "monument") return 3;
    if (cat == "bush" || cat == "rock" || cat == "log") return 3;
    if (cat == "fence" || cat == "rail" || cat == "wall" || cat == "fern") return 2;
    return 1;
}

Asset build_asset(const std::string& cat, int v) {
    Asset a;
    if (cat == "house") {
        add_prop(a, PropLibrary::build_house(static_cast<u32>(v)));
    } else if (cat == "townhouse") {
        add_prop(a, PropLibrary::build_townhouse());
    } else if (cat == "pub") {
        add_prop(a, PropLibrary::build_pub());
    } else if (cat == "blacksmith") {
        add_prop(a, PropLibrary::build_blacksmith());
    } else if (cat == "tree") {
        primitives::TreeMeshData tm = primitives::tree(v);
        a.parts.push_back({std::move(tm.trunk), Vec4{1.0f}});
        a.parts.push_back({std::move(tm.foliage), Vec4{tree_foliage_tint(v), 1.0f}});
    } else if (cat == "bush") {
        add_prop(a, PropLibrary::build_bush(v));
    } else if (cat == "rock") {
        add_prop(a, PropLibrary::build_rock(v));
    } else if (cat == "log") {
        add_prop(a, PropLibrary::build_log(v));
    } else if (cat == "fence") {
        add_prop(a, PropLibrary::build_fence(v));
    } else if (cat == "rail") {
        add_prop(a, PropLibrary::build_fence_rail(v));
    } else if (cat == "lantern") {
        add_prop(a, PropLibrary::build_lantern_post());
    } else if (cat == "well") {
        add_prop(a, PropLibrary::build_well());
    } else if (cat == "gate") {
        add_prop(a, PropLibrary::build_gate());
    } else if (cat == "tower") {
        add_prop(a, PropLibrary::build_tower());
    } else if (cat == "wall") {
        add_prop(a, PropLibrary::build_wall(v));
    } else if (cat == "market") {
        add_prop(a, PropLibrary::build_market());
    } else if (cat == "fountain") {
        add_prop(a, PropLibrary::build_fountain());
    } else if (cat == "planter") {
        add_prop(a, PropLibrary::build_planter());
    } else if (cat == "bridge") {
        add_prop(a, PropLibrary::build_bridge());
    } else if (cat == "stonebridge") {
        add_prop(a, PropLibrary::build_stone_bridge());
    } else if (cat == "path") {
        add_prop(a, PropLibrary::build_path_tile());
    } else if (cat == "river") {
        add_prop(a, PropLibrary::build_river());
    } else if (cat == "decor") {
        add_prop(a, PropLibrary::build_decor(v));
    } else if (cat == "crystal") {
        add_prop(a, PropLibrary::build_crystal(v));
    } else if (cat == "glowshroom") {
        add_prop(a, PropLibrary::build_glow_shroom(v));
    } else if (cat == "campfire") {
        add_prop(a, PropLibrary::build_campfire());
    } else if (cat == "monument") {
        add_prop(a, PropLibrary::build_monument(v));
    } else if (cat == "watchtower") {
        add_prop(a, PropLibrary::build_watchtower());
    } else if (cat == "wagon") {
        add_prop(a, PropLibrary::build_wagon());
    } else if (cat == "wheel") {
        add_prop(a, PropLibrary::build_wagon_wheel());
    } else if (cat == "ox") {
        a.parts.push_back({build_ox_body(), Vec4{1.0f}});
        for (const Vec3& lo : kOxLegs) {
            MeshData leg = build_ox_leg();
            for (Vertex& v : leg.vertices) v.position += lo;
            a.parts.push_back({std::move(leg), Vec4{1.0f}});
        }
    } else if (cat == "deer") {
        a.parts.push_back({build_deer_body(), Vec4{1.0f}});
        for (const Vec3& lo : kDeerLegs) {
            MeshData leg = build_deer_leg();
            for (Vertex& v : leg.vertices) v.position += lo;
            a.parts.push_back({std::move(leg), Vec4{1.0f}});
        }
    } else if (cat == "fern") {
        a.parts.push_back({primitives::fern(v), Vec4{1.0f}});
    } else if (cat == "mushroom") {
        a.parts.push_back({primitives::mushroom(), Vec4{1.0f}});
    }
    return a;
}

// Combined local-space AABB of an asset's parts.
void asset_bounds(const Asset& a, Vec3& lo, Vec3& hi) {
    lo = Vec3{1e9f};
    hi = Vec3{-1e9f};
    for (const AssetPart& p : a.parts) {
        for (const Vertex& v : p.mesh.vertices) {
            lo = glm::min(lo, v.position);
            hi = glm::max(hi, v.position);
        }
    }
    if (a.parts.empty() || lo.x > hi.x) { // empty asset -> a unit box
        lo = Vec3{-0.5f};
        hi = Vec3{0.5f};
    }
}

bool render_one(test::OffscreenRenderer& r, const std::string& cat, int v, const std::string& out) {
    Asset asset = build_asset(cat, v);
    if (asset.parts.empty()) {
        ALRYN_ERROR("Unknown / empty asset '{}' variant {}", cat, v);
        return false;
    }
    Vec3 lo, hi;
    asset_bounds(asset, lo, hi);
    const Vec3 center{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    const f32 radius = std::max(0.6f, glm::length(hi - lo) * 0.5f);

    // A muted grass ground plane the asset sits on (so the lighting + footprint read).
    const f32 g = std::max(radius * 2.2f, 6.0f);
    MeshData ground = primitives::box(Vec3{-g, -0.06f, -g}, Vec3{g, lo.y, g}, Vec3{0.40f, 0.50f, 0.30f});

    std::vector<test::OffscreenRenderer::Draw> draws;
    draws.push_back({r.upload(ground), Mat4{1.0f}, Vec4{1.0f}});
    for (AssetPart& p : asset.parts) {
        if (Mesh* m = r.upload(p.mesh)) {
            draws.push_back({m, Mat4{1.0f}, p.tint});
        }
    }

    // A 3/4 iso camera framing the asset's bounding sphere, looking at its centre.
    const f32 aspect = static_cast<f32>(r.width()) / static_cast<f32>(r.height());
    const f32 fov = radians(34.0f);
    const f32 fit = std::min(fov, 2.0f * std::atan(std::tan(fov * 0.5f) * aspect));
    const f32 dist = radius / std::sin(fit * 0.5f) * 1.18f;
    const Vec3 dir = glm::normalize(Vec3{0.62f, 0.52f, 0.62f});
    const Vec3 eye = center + dir * dist;
    Mat4 proj = perspective(fov, aspect, 0.1f, dist * 4.0f);
    Mat4 view = look_at(eye, center, Vec3{0.0f, 1.0f, 0.0f});

    const Vec3 sky{0.52f, 0.60f, 0.70f};               // soft sky backdrop
    const Vec3 sun = glm::normalize(Vec3{0.4f, 0.86f, 0.3f}); // a high key light from front-right
    r.render(draws, view, proj, sky, sun, out);
    ALRYN_INFO("wrote {}  ({}x{}, asset r={:.1f}m)", out, r.width(), r.height(), radius);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: asset_preview <category> [variant] [out.ppm]\n");
        std::printf("  categories: house townhouse pub blacksmith tree bush rock log fence rail "
                    "lantern well gate tower wall market fountain planter bridge stonebridge path "
                    "river decor wagon wheel fern mushroom\n");
        std::printf("  no variant -> render every variant of the category to its own file\n");
        return 2;
    }
    const std::string cat = argv[1];

    test::OffscreenRenderer renderer;
    if (!renderer.init(900, 680)) {
        ALRYN_ERROR("Offscreen renderer unavailable (no Vulkan device or shaders not compiled).");
        return 1;
    }

    if (argc >= 3) {
        const int v = std::atoi(argv[2]);
        const std::string out =
            argc >= 4 ? argv[3] : ("asset_" + cat + "_" + std::to_string(v) + ".ppm");
        return render_one(renderer, cat, v, out) ? 0 : 1;
    }

    // No variant: render them all.
    const int n = variant_count(cat);
    bool ok = true;
    for (int v = 0; v < n; ++v) {
        ok &= render_one(renderer, cat, v, "asset_" + cat + "_" + std::to_string(v) + ".ppm");
    }
    return ok ? 0 : 1;
}
