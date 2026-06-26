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

#include <Alryn/Character/BodyMesh.h>
#include <Alryn/Character/CharacterAnimator.h>
#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Character/ClothRig.h>
#include <Alryn/Character/Equipment.h>
#include <Alryn/Character/Outfit.h>
#include <Alryn/Character/OutfitMesh.h>
#include <Alryn/Character/SkinnedMesh.h>
#include <Alryn/Character/Weapon.h>
#include <Alryn/Core/Log.h>
#include <Alryn/Game/Roles.h>
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
    if (cat == "character") return kRoleCount + 1; // one per role + a Peasant NPC (role index kRoleCount)
    if (cat == "house") return 8;
    if (cat == "tree") return 5;
    if (cat == "decor") return 8;
    if (cat == "crystal") return 4;
    if (cat == "monument") return 3;
    if (cat == "cactus") return 2;
    if (cat == "bush" || cat == "rock" || cat == "log") return 3;
    if (cat == "fence" || cat == "rail" || cat == "wall" || cat == "fern") return 2;
    return 1;
}

Asset build_character(int role); // defined below

Asset build_asset(const std::string& cat, int v) {
    Asset a;
    if (cat == "character") {
        return build_character(v); // v = role (0 Knight / 1 Hunter / 2 Cleric / 3 Mage)
    }
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
    } else if (cat == "archbridge") {
        add_prop(a, PropLibrary::build_arch_bridge());
    } else if (cat == "plankbridge") {
        add_prop(a, PropLibrary::build_plank_bridge());
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
    } else if (cat == "cargo") {
        a.parts.push_back({primitives::crate(Vec3{-0.22f, 0.0f, -0.22f}, Vec3{0.22f, 0.44f, 0.22f},
                                             Vec3{0.55f, 0.40f, 0.22f}),
                           Vec4{1.0f}});
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
    } else if (cat == "horse") {
        a.parts.push_back({build_horse_body(), Vec4{1.0f}});
        for (const Vec3& lo : kHorseLegs) {
            MeshData leg = build_horse_leg();
            for (Vertex& v : leg.vertices) v.position += lo;
            a.parts.push_back({std::move(leg), Vec4{1.0f}});
        }
    } else if (cat == "fern") {
        a.parts.push_back({primitives::fern(v), Vec4{1.0f}});
    } else if (cat == "mushroom") {
        a.parts.push_back({primitives::mushroom(), Vec4{1.0f}});
    } else if (cat == "reed") {
        a.parts.push_back({primitives::reed(5), Vec4{1.0f}});
    } else if (cat == "cactus") {
        a.parts.push_back({primitives::cactus(v), Vec4{1.0f}});
    }
    return a;
}

// Bake a unit shape mesh, transformed by `m` and recoloured to `color`, into world space (so the
// character's posed rig becomes a flat list of parts the previewer frames + renders like any asset).
MeshData bake(const MeshData& src, const Mat4& m, const Vec3& color) {
    MeshData out = src;
    const Mat3 nrm{glm::transpose(glm::inverse(Mat3{m}))};
    for (Vertex& v : out.vertices) {
        v.position = Vec3{m * Vec4{v.position, 1.0f}};
        v.normal = glm::normalize(nrm * v.normal);
        v.color = color;
    }
    return out;
}

// The player character for a role, baked from its posed rig in the bind pose - so the headless
// preview matches the in-game rig (same primitives the client uploads to shape_*_). This is the
// iteration harness for the character overhaul: it'll grow role-themed outfits + weapons.
Asset build_character(int role) {
    // Unit shapes matching ClientApp's shape_box_/sphere_/cylinder_/capsule_/rounded_.
    const MeshData box = primitives::cube(1.0f, Vec3{1.0f});
    const MeshData sphere = primitives::sphere(18, 12, Vec3{1.0f});
    const MeshData cylinder = primitives::cylinder(16, Vec3{1.0f});
    const MeshData capsule = primitives::capsule(18, 6, Vec3{1.0f});
    const MeshData rounded = primitives::rounded_box(0.32f, Vec3{1.0f});

    CharacterAppearance app; // a plain face/body; the role-themed outfit goes on top
    const u32 seed = 1000u + static_cast<u32>(role);
    CharacterModel model = CharacterModel::create(seed, app);
    // Equip a role-flavoured outfit so the preview matches the in-game render (skinned body + the
    // outfit/face attachment primitives). ALRYN_TIER (0..3) overrides the ragged->master tier.
    static const u8 role_tint[kRoleCount] = {0, 2, 5, 3}; // Knight blue, Hunter green, Cleric white, Mage violet
    u8 tier = 3;
    if (const char* t = std::getenv("ALRYN_TIER")) {
        tier = static_cast<u8>(glm::clamp(std::atoi(t), 0, 3));
    }
    // role == kRoleCount renders a generic Peasant NPC (no weapon).
    const bool peasant = role >= kRoleCount;
    const OutfitKind okind = peasant ? OutfitKind::Peasant : outfit_kind_for_role(static_cast<u8>(role));
    Equipment eq;
    eq.outfit_tier = tier;
    eq.weapon_tier = tier;
    eq.outfit_tint = peasant ? 0u : role_tint[static_cast<usize>(role) % kRoleCount];
    apply_outfit(model, okind, eq);

    // Pose: bind by default (arms at the sides). Set ALRYN_POSE=walk|swing|cast|block|idle to drive
    // the animator mid-action and verify the locomotion + action BLEND, or the standing idle stance.
    std::vector<Quat> pose;          // empty => bind pose
    Mat4 root{1.0f};
    bool idle_mode = false;
    bool cloth_mode = false;
    if (const char* mode = std::getenv("ALRYN_POSE")) {
        const std::string m = mode;
        if (m == "cloth") {
            cloth_mode = true; // bind pose + a simulated cape settled under gravity + wind
        } else if (m == "idle") {
            // The standing idle stance (mirrors ClientApp::apply_idle_stance): a staff/mace user (Cleric
            // role 2 / Mage role 3) rests on a planted weapon, the Hunter (role 1) lowers the bow.
            idle_mode = true;
            pose.assign(model.bone_count(), QuatIdentity);
            auto setq = [&](BonePart p, const Quat& q) {
                const int i = model.bone_index(p);
                if (i >= 0) pose[static_cast<usize>(i)] = q;
            };
            const Vec3 X{1.0f, 0.0f, 0.0f}, Z{0.0f, 0.0f, 1.0f};
            if (role == 2 || role == 3) {
                setq(BonePart::UpperArmL, glm::angleAxis(-0.62f, X) * glm::angleAxis(-0.18f, Z));
                setq(BonePart::LowerArmL, glm::angleAxis(0.95f, X));
            } else if (role == 1) {
                setq(BonePart::UpperArmL, glm::angleAxis(-0.32f, X));
                setq(BonePart::LowerArmL, glm::angleAxis(0.28f, X));
            }
        } else {
            CharacterAnimator anim;
            const Timestep dt{1.0f / 60.0f};
            for (int k = 0; k < 40; ++k) {
                anim.update(5.0f, dt); // walk up to a mid-stride
            }
            int action_frames = 13; // ALRYN_FRAMES overrides, to sweep an action's phase
            if (const char* af = std::getenv("ALRYN_FRAMES")) {
                action_frames = std::atoi(af);
            }
            if (m == "swing") {
                anim.play_swing();
                for (int k = 0; k < action_frames; ++k) anim.update(5.0f, dt);
            } else if (m == "cast") {
                anim.play_cast();
                for (int k = 0; k < 17; ++k) anim.update(5.0f, dt); // ~mid-thrust
            } else if (m == "block") {
                anim.set_blocking(true);
                for (int k = 0; k < 20; ++k) anim.update(5.0f, dt);
            }
            pose = anim.pose(model);
            root = anim.body_offset();
        }
    }
    // ALRYN_YAW (degrees) spins the character on the spot - handy to view an action in profile.
    if (const char* yw = std::getenv("ALRYN_YAW")) {
        root = glm::rotate(Mat4{1.0f}, radians(static_cast<f32>(std::atof(yw))), Vec3{0.0f, 1.0f, 0.0f}) * root;
    }
    const std::vector<Mat4> jmats = model.joint_matrices(root, pose);
    const CharacterPalette& pal = model.palette();
    const std::vector<Bone>& bones = model.bones();

    // Trace the sword arm's pointing direction (down the forearm, local -Y) in world space, so the
    // swing arc can be checked numerically: +z = the character's FORWARD/facing, +y = up.
    if (std::getenv("ALRYN_DEBUG_TIP") != nullptr) {
        const int bi = model.bone_index(BonePart::LowerArmL);
        if (bi >= 0) {
            const Vec3 down{-jmats[static_cast<usize>(bi)][1]}; // -local-Y axis = where the forearm/blade points
            std::printf("blade dir  y=%+.2f  z=%+.2f  (%s, %s)\n", down.y, down.z,
                        down.z > 0.1f ? "FRONT" : down.z < -0.1f ? "BACK" : "mid",
                        down.y > 0.1f ? "up" : down.y < -0.1f ? "down" : "level");
        }
    }

    auto shape_for = [&](BoneShape s) -> const MeshData& {
        return s == BoneShape::Sphere       ? sphere
               : s == BoneShape::Cylinder   ? cylinder
               : s == BoneShape::Capsule    ? capsule
               : s == BoneShape::RoundedBox ? rounded
                                            : box;
    };

    // The continuous skinned body + the continuous skinned outfit (armour/cloth that flows across the
    // joints), both vertex-weighted to the bones and deformed by the posed joint frames (linear-blend
    // skinning). Materials resolve to the character palette at skin time.
    Asset a;
    auto palette = [&](u8 mat) -> Vec3 { return body_material_color(pal, static_cast<BodyMaterial>(mat)); };
    auto add_skinned = [&](const SkinnedMesh& src) {
        if (src.vertices.empty()) {
            return;
        }
        MeshData md;
        skin(src, jmats, md.vertices, palette);
        md.indices = src.indices;
        a.parts.push_back({std::move(md), Vec4{1.0f}});
    };
    add_skinned(build_body_mesh(model));
    add_skinned(build_outfit_mesh(model, okind, eq));

    // Face/hair + outfit pieces ride ON TOP of the skinned body as primitives (Bone::attachment),
    // exactly as the client's draw_rig(attachments_only) lays them over the skinned mesh.
    auto bone_color = [&](BoneColor c) -> Vec3 {
        switch (c) {
            case BoneColor::Skin: return pal.skin;
            case BoneColor::Shirt: return pal.shirt;
            case BoneColor::Pants: return pal.pants;
            case BoneColor::Hair: return pal.hair;
            case BoneColor::Eye: return pal.eye;
            case BoneColor::Primary: return pal.primary;
            case BoneColor::Accent: return pal.accent;
            case BoneColor::Metal: return pal.metal;
            case BoneColor::Dark: return pal.dark;
            case BoneColor::Glow: return pal.glow * 1.7f; // brighten (no emissive pass in the preview)
        }
        return Vec3{1.0f};
    };
    const std::vector<Mat4> mats = model.bone_matrices(root, pose);
    for (usize i = 0; i < bones.size(); ++i) {
        if (!bones[i].attachment) {
            continue; // core body + joint fillers are the skinned mesh
        }
        a.parts.push_back({bake(shape_for(bones[i].shape), mats[i], bone_color(bones[i].color)), Vec4{1.0f}});
    }

    // Weapon(s) in hand: the L-suffixed arm is the player's RIGHT (main hand), R-suffixed the left
    // (off hand). Bake the modular weapon pieces at those hand-joint frames.
    auto hand_frame = [&](BonePart arm) -> Mat4 {
        const int bi = model.bone_index(arm);
        if (bi < 0) {
            return Mat4{1.0f};
        }
        const f32 wrist = bones[static_cast<usize>(bi)].box_center.y * 2.0f;
        return jmats[static_cast<usize>(bi)] * glm::translate(Mat4{1.0f}, Vec3{0.0f, wrist, 0.0f});
    };
    auto add_weapon = [&](WeaponType wt, const Mat4& hand) {
        for (const WeaponPiece& wp : weapon_pieces(wt, EquipmentTier::Master, pal)) {
            const Vec3 c = wp.emissive ? wp.color * 1.7f : wp.color;
            a.parts.push_back({bake(shape_for(wp.shape), hand * wp.local, c), Vec4{1.0f}});
        }
    };
    const bool staff_user = (role == 2 || role == 3);
    if (!peasant && idle_mode && staff_user) {
        // A planted vertical staff/mace the idle character rests on (mirrors draw_planted_weapon).
        const Mat4 hand = hand_frame(BonePart::LowerArmL);
        const Vec3 grip = Vec3{hand[3]};
        const Vec3 bottom{grip.x, 0.0f, grip.z};        // stand it on the ground (y=0)
        const Vec3 top = grip + Vec3{0.0f, 0.5f, 0.0f}; // a TALL shaft rising past the hand
        const f32 len = std::max(0.6f, top.y - bottom.y);
        const Vec3 mid = (top + bottom) * 0.5f;
        a.parts.push_back({bake(box, glm::translate(Mat4{1.0f}, mid) * // vertical shaft (planted)
                                          glm::scale(Mat4{1.0f}, Vec3{0.045f, len, 0.045f}),
                                Vec3{0.42f, 0.29f, 0.16f}),
                           Vec4{1.0f}});
        a.parts.push_back({bake(sphere, glm::translate(Mat4{1.0f}, top) *
                                            glm::scale(Mat4{1.0f}, Vec3{role == 3 ? 0.15f : 0.13f}),
                                role == 3 ? pal.glow * 1.7f : pal.accent),
                           Vec4{1.0f}});
        if (role_offhand(static_cast<u8>(role)) != WeaponType::None) {
            add_weapon(role_offhand(static_cast<u8>(role)), hand_frame(BonePart::LowerArmR));
        }
    } else if (!peasant) {
        add_weapon(role_weapon(static_cast<u8>(role), 0), hand_frame(BonePart::LowerArmL));
        if (role_offhand(static_cast<u8>(role)) != WeaponType::None) {
            add_weapon(role_offhand(static_cast<u8>(role)), hand_frame(BonePart::LowerArmR));
        }
    }

    // Simulated cloth (cape sheet + robe-skirt tube) settled under gravity + a light wind - proves the
    // sim + meshes (ALRYN_POSE=cloth). Mirrors ClientApp::setup_cloth's pieces.
    if (cloth_mode) {
        if (std::getenv("ALRYN_MEASURE") != nullptr) {
            f32 r_torso = 0.0f, r_leg = 0.0f, z_back = 0.0f;
            for (const AssetPart& p : a.parts) {
                for (const Vertex& vv : p.mesh.vertices) {
                    const f32 r = std::sqrt(vv.position.x * vv.position.x + vv.position.z * vv.position.z);
                    if (vv.position.y > 0.85f && vv.position.y < 1.1f) {
                        r_torso = std::max(r_torso, r);
                        if (std::abs(vv.position.x) < 0.05f) z_back = std::min(z_back, vv.position.z);
                    }
                    if (vv.position.y > 0.25f && vv.position.y < 0.5f) r_leg = std::max(r_leg, r);
                }
            }
            std::printf("body+outfit: torso r=%.3f  legs r=%.3f  back z=%.3f\n", r_torso, r_leg, z_back);
        }
        const Vec3 wind{2.2f, 0.0f, -1.0f};
        const int iH = model.bone_index(BonePart::Head), iT = model.bone_index(BonePart::Torso);
        const Vec3 neck = iH >= 0 ? Vec3{jmats[static_cast<usize>(iH)][3]} : Vec3{0.0f, 1.0f, 0.0f};
        const Vec3 torso = iT >= 0 ? Vec3{jmats[static_cast<usize>(iT)][3]} : Vec3{0.0f, 0.6f, 0.0f};
        const bool cut = std::getenv("ALRYN_CUT") != nullptr;
        // Body collision: push nodes out to radius `r` (>= body+outfit surface ~0.27-0.3) so cloth
        // rests ON the body instead of inside it. Mirrors ClientApp::draw_cloth.
        auto collide_body = [&](ClothChain& s, f32 r) {
            for (usize n = 1; n < s.pos.size(); ++n) {
                for (Vec3* pp : {&s.pos[n], &s.prev[n]}) {
                    if (pp->y < 0.0f || pp->y > 1.25f) continue;
                    const f32 d2 = pp->x * pp->x + pp->z * pp->z;
                    if (d2 < r * r && d2 > 1e-6f) {
                        const f32 d = std::sqrt(d2);
                        pp->x = pp->x / d * r;
                        pp->z = pp->z / d * r;
                    }
                }
            }
        };
        auto make_sheet = [&](const Vec3& anchor, const Vec3& hang, int segs, f32 seg, f32 width,
                              const Vec3& color) {
            ClothChain s;
            s.init(anchor, hang, segs, seg, width);
            for (int k = 0; k < 160; ++k) {
                s.step(anchor, wind, 9.0f, 1.0f / 60.0f);
                collide_body(s, 0.3f);
            }
            if (cut) {
                s.detach();
                for (Vec3& pp : s.prev) pp = pp - Vec3{0.05f, 0.04f, -0.05f};
                for (int k = 0; k < 32; ++k) s.step(anchor, wind, 9.0f, 1.0f / 60.0f);
            }
            MeshData cm;
            build_cloth_mesh(s, Vec3{1.0f, 0.0f, 0.0f}, color, cm);
            a.parts.push_back({std::move(cm), Vec4{1.0f}});
        };
        // Cape on cape-wearing roles: a wide multi-panel cloak on a shallow ARC across the upper back
        // (open sheet), standing clear of the body (mirrors ClientApp::add_cape).
        if (role == 0 || role == 2 || role == 1) {
            constexpr int kN = 6;
            constexpr f32 arc = 0.85f;
            auto cape_anchor = [&](int i) {
                const f32 a = glm::mix(-arc, arc, static_cast<f32>(i) / static_cast<f32>(kN - 1));
                return neck + Vec3{std::sin(a), 0.0f, -std::cos(a)} * 0.30f + Vec3{0.0f, 0.05f, 0.0f};
            };
            std::vector<ClothChain> cape(kN);
            for (int i = 0; i < kN; ++i) {
                const f32 a = glm::mix(-arc, arc, static_cast<f32>(i) / static_cast<f32>(kN - 1));
                const Vec3 dir{std::sin(a), 0.0f, -std::cos(a)};
                cape[static_cast<usize>(i)].init(cape_anchor(i),
                                                 glm::normalize(dir * 0.4f + Vec3{0.0f, -1.0f, 0.0f}), 6,
                                                 0.12f, 0.0f);
            }
            for (int k = 0; k < 170; ++k) {
                for (int i = 0; i < kN; ++i) {
                    cape[static_cast<usize>(i)].step(cape_anchor(i), wind, 9.0f, 1.0f / 60.0f);
                    collide_body(cape[static_cast<usize>(i)], 0.36f);
                }
            }
            if (cut) {
                for (ClothChain& ch : cape) {
                    ch.detach();
                }
                for (int k = 0; k < 36; ++k) {
                    for (ClothChain& ch : cape) {
                        ch.step(Vec3{0.0f}, wind, 9.0f, 1.0f / 60.0f);
                    }
                }
            }
            MeshData cm;
            build_cloth_tube(cape, false, role == 1 ? pal.dark : pal.primary, cm);
            a.parts.push_back({std::move(cm), Vec4{1.0f}});
        }
        if (role == 2) { // stole - two front bands
            for (f32 ex : {-1.0f, 1.0f}) {
                make_sheet(torso + Vec3{ex * 0.09f, 0.42f, 0.1f}, Vec3{0.0f, -1.0f, 0.05f}, 4, 0.12f, 0.045f, pal.dark);
            }
        }
        if (role == 1) { // warden's short shoulder mantle
            make_sheet(neck + Vec3{0.0f, -0.02f, -0.26f}, Vec3{0.0f, -1.0f, -0.32f}, 3, 0.1f, 0.28f, pal.dark);
        }
        // A flowing lower garment (ring tube) on EVERY role: robe (Mage/Cleric), surcoat (Knight),
        // tunic skirt (Hunter).
        {
            const int iP = model.bone_index(BonePart::Pelvis);
            const Vec3 hip = iP >= 0 ? Vec3{jmats[static_cast<usize>(iP)][3]} : Vec3{0.0f, 0.6f, 0.0f};
            const int segs = role == 2 ? 7 : role == 3 ? 6 : role == 0 ? 4 : 3;
            const f32 seg = (role == 2 || role == 3) ? 0.15f : role == 0 ? 0.13f : 0.12f;
            const f32 flare = role == 2 ? 0.3f : role == 3 ? 0.34f : role == 0 ? 0.26f : 0.3f;
            const f32 radius = role == 0 ? 0.19f : 0.17f;
            constexpr int kN = 8;
            std::vector<ClothChain> skirt(kN);
            for (int i = 0; i < kN; ++i) {
                const f32 ang = TwoPi * static_cast<f32>(i) / static_cast<f32>(kN);
                const Vec3 radial{std::sin(ang), 0.0f, std::cos(ang)};
                const Vec3 anchor = hip + radial * radius + Vec3{0.0f, 0.06f, 0.0f};
                skirt[static_cast<usize>(i)].init(anchor, glm::normalize(radial * flare + Vec3{0, -1, 0}),
                                                  segs, seg, 0.0f);
            }
            // Push hanging nodes out of the body cylinder (mirrors ClientApp::draw_cloth's collision).
            auto collide = [&](Vec3& p) {
                if (p.y < 0.0f || p.y > hip.y + 0.35f) return;
                const f32 dx = p.x - hip.x, dz = p.z - hip.z, d2 = dx * dx + dz * dz;
                if (d2 < 0.28f * 0.28f && d2 > 1e-6f) {
                    const f32 d = std::sqrt(d2);
                    p.x = hip.x + dx / d * 0.28f;
                    p.z = hip.z + dz / d * 0.28f;
                }
            };
            for (int k = 0; k < 160; ++k) {
                for (int i = 0; i < kN; ++i) {
                    const f32 ang = TwoPi * static_cast<f32>(i) / static_cast<f32>(kN);
                    const Vec3 radial{std::sin(ang), 0.0f, std::cos(ang)};
                    const Vec3 anchor = hip + radial * radius + Vec3{0.0f, 0.06f, 0.0f};
                    ClothChain& ch = skirt[static_cast<usize>(i)];
                    ch.step(anchor, wind, 9.0f, 1.0f / 60.0f);
                    for (usize n = 1; n < ch.pos.size(); ++n) {
                        collide(ch.pos[n]);
                        collide(ch.prev[n]);
                    }
                }
            }
            MeshData sm;
            build_cloth_tube(skirt, true, pal.primary, sm);
            a.parts.push_back({std::move(sm), Vec4{1.0f}});
        }
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
    // Characters face local +Z; frame them from a near eye-level 3/4 (like the reference art) rather
    // than the high iso angle used for props.
    const Vec3 dir = (cat == "character") ? glm::normalize(Vec3{0.34f, 0.13f, 1.0f})
                                          : glm::normalize(Vec3{0.62f, 0.52f, 0.62f});
    const Vec3 eye = center + dir * dist;
    Mat4 proj = perspective(fov, aspect, 0.1f, dist * 4.0f);
    Mat4 view = look_at(eye, center, Vec3{0.0f, 1.0f, 0.0f});

    // A brighter sky (more ambient fill) + a lower, more frontal key light for characters, so vertical
    // armour/cloth surfaces catch light and read like the reference art (props keep the high iso sun).
    const bool character = (cat == "character");
    const Vec3 sky = character ? Vec3{0.66f, 0.72f, 0.80f} : Vec3{0.52f, 0.60f, 0.70f};
    const Vec3 sun = character ? glm::normalize(Vec3{0.38f, 0.5f, 0.78f})
                               : glm::normalize(Vec3{0.4f, 0.86f, 0.3f});
    // Characters: a dimmer, warmer key (intensity 0.72, warm white) so steel/cloth read with proper
    // tonal contrast instead of washing toward white - closer to the moodier in-game tonemap. Props keep
    // the original full white key (default) so their scene-shot baselines are unchanged.
    const Vec4 key = character ? Vec4{1.0f, 0.95f, 0.86f, 0.72f} : Vec4{1.0f};
    r.render(draws, view, proj, sky, sun, out, key);
    ALRYN_INFO("wrote {}  ({}x{}, asset r={:.1f}m)", out, r.width(), r.height(), radius);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: asset_preview <category> [variant] [out.ppm]\n");
        std::printf("  categories: character house townhouse pub blacksmith tree bush rock log fence rail "
                    "lantern well gate tower wall market fountain planter bridge stonebridge "
                    "archbridge plankbridge path river decor wagon wheel fern mushroom\n");
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
