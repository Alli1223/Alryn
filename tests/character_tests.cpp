#include <doctest/doctest.h>

#include <Alryn/Character/BodyMesh.h>
#include <Alryn/Character/CharacterAnimator.h>
#include <Alryn/Character/ClothRig.h>
#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Character/Equipment.h>
#include <Alryn/Character/Outfit.h>
#include <Alryn/Character/OutfitMesh.h>
#include <Alryn/Character/SkinnedMesh.h>

#include <algorithm>
#include <cmath>

using namespace alryn;

TEST_CASE("SkinnedMesh: linear-blend skinning deforms vertices with the bones") {
    // Two bones, both bound at the identity (inverse-bind = identity).
    SkinnedMesh m;
    m.inverse_bind = {Mat4{1.0f}, Mat4{1.0f}};

    SkinVertex a; // fully weighted to bone 1
    a.position = Vec3{1.0f, 0.0f, 0.0f};
    a.set_weights({{1, 1.0f}});
    SkinVertex b; // 50/50 between bone 0 (identity) and bone 1
    b.position = Vec3{1.0f, 0.0f, 0.0f};
    b.set_weights({{0, 1.0f}, {1, 1.0f}});
    m.add_vertex(a);
    m.add_vertex(b);

    std::vector<Vertex> out;

    // Bind pose (both joints identity): vertices stay put.
    skin(m, {Mat4{1.0f}, Mat4{1.0f}}, out);
    REQUIRE(out.size() == 2);
    CHECK(glm::length(out[0].position - Vec3{1.0f, 0.0f, 0.0f}) < 1e-4f);

    // Rotate bone 1 by +90deg about Z: (1,0,0) -> (0,1,0).
    const Mat4 rot = glm::rotate(Mat4{1.0f}, HalfPi, Vec3{0.0f, 0.0f, 1.0f});
    skin(m, {Mat4{1.0f}, rot}, out);
    CHECK(glm::length(out[0].position - Vec3{0.0f, 1.0f, 0.0f}) < 1e-3f); // follows bone 1 fully
    // The 50/50 vertex lands between (1,0,0) [bone 0] and (0,1,0) [bone 1] - the LBS average.
    CHECK(out[1].position.x > 0.1f);
    CHECK(out[1].position.x < 0.9f);
    CHECK(out[1].position.y > 0.1f);
    CHECK(out[1].position.y < 0.9f);

    // A translating bone carries its vertices.
    const Mat4 trans = glm::translate(Mat4{1.0f}, Vec3{0.0f, 2.0f, 0.0f});
    skin(m, {Mat4{1.0f}, trans}, out);
    CHECK(glm::length(out[0].position - Vec3{1.0f, 2.0f, 0.0f}) < 1e-4f);

    // The palette resolves the material id to a colour.
    bool called = false;
    skin(m, {Mat4{1.0f}, Mat4{1.0f}}, out, [&](u8) {
        called = true;
        return Vec3{0.2f, 0.4f, 0.6f};
    });
    CHECK(called);
    CHECK(out[0].color == Vec3{0.2f, 0.4f, 0.6f});
}

TEST_CASE("BodyMesh/OutfitMesh: skinned body + outfit are valid and deform with the bones") {
    CharacterAppearance app;
    CharacterModel model = CharacterModel::create(7u, app);

    const SkinnedMesh body = build_body_mesh(model);
    REQUIRE(body.vertices.size() > 0);
    REQUIRE(body.indices.size() % 3 == 0);
    REQUIRE(body.inverse_bind.size() == model.bone_count());
    // Every vertex references valid bones and carries a normalised weight set.
    for (const SkinVertex& v : body.vertices) {
        f32 wsum = 0.0f;
        for (int k = 0; k < kMaxInfluences; ++k) {
            CHECK(v.bones[k] >= 0);
            CHECK(v.bones[k] < static_cast<int>(model.bone_count()));
            wsum += v.weights[k];
        }
        CHECK(wsum == doctest::Approx(1.0f));
    }

    // Skinned at the bind pose, the body spans roughly the character's height (feet ~0, head ~height).
    const std::vector<Mat4> bind = model.joint_matrices(Mat4{1.0f}, {});
    std::vector<Vertex> out;
    skin(body, bind, out);
    REQUIRE(out.size() == body.vertices.size());
    f32 lo = 1e9f, hi = -1e9f;
    for (const Vertex& v : out) {
        lo = std::min(lo, v.position.y);
        hi = std::max(hi, v.position.y);
    }
    CHECK(lo < 0.3f);                  // feet near the ground
    CHECK(hi > model.height() * 0.7f); // reaches up toward the head

    // A posed joint moves the skinned surface: rotating the whole character lifts nothing but a bent
    // knee pose should shift some lower-leg vertices relative to bind.
    std::vector<Quat> pose(model.bone_count(), QuatIdentity);
    const int knee = model.bone_index(BonePart::LowerLegL);
    REQUIRE(knee >= 0);
    pose[static_cast<usize>(knee)] = glm::angleAxis(0.6f, Vec3{1.0f, 0.0f, 0.0f});
    std::vector<Vertex> posed;
    skin(body, model.joint_matrices(Mat4{1.0f}, pose), posed);
    f32 max_shift = 0.0f;
    for (usize i = 0; i < out.size(); ++i) {
        max_shift = std::max(max_shift, glm::length(posed[i].position - out[i].position));
    }
    CHECK(max_shift > 0.02f); // the knee bend visibly deforms the mesh

    // The outfit mesh is built per role, valid, and weighted to the same skeleton.
    for (OutfitKind kind : {OutfitKind::Plate, OutfitKind::Robe, OutfitKind::Leather, OutfitKind::Holy}) {
        Equipment eq;
        eq.outfit_tier = 3;
        CharacterModel m2 = CharacterModel::create(7u, app);
        apply_outfit(m2, kind, eq);
        const SkinnedMesh outfit = build_outfit_mesh(m2, kind, eq);
        REQUIRE(outfit.vertices.size() > 0);
        REQUIRE(outfit.indices.size() % 3 == 0);
        REQUIRE(outfit.inverse_bind.size() == m2.bone_count());
        for (const SkinVertex& v : outfit.vertices) {
            for (int k = 0; k < kMaxInfluences; ++k) {
                CHECK(v.bones[k] >= 0);
                CHECK(v.bones[k] < static_cast<int>(m2.bone_count()));
            }
        }
    }
}

TEST_CASE("Character: races reproportion the body and add signature features") {
    auto make = [](Race race) {
        CharacterAppearance a;
        a.race = race;
        a.hair = HairStyle::Bald; // uncover the head so the beard/ear features are countable
        return CharacterModel::create(5u, a);
    };
    const CharacterModel man = make(Race::Human);
    const CharacterModel dwarf = make(Race::Dwarf);
    const CharacterModel elf = make(Race::Elf);

    // Dwarves are shorter than men; elves are taller.
    CHECK(dwarf.height() < man.height());
    CHECK(elf.height() > man.height());

    // Dwarves are broader, elves slimmer (the pelvis width scales with the race build).
    auto pelvis_w = [](const CharacterModel& m) {
        return m.bones()[static_cast<usize>(m.bone_index(BonePart::Pelvis))].box_size.x;
    };
    CHECK(pelvis_w(dwarf) > pelvis_w(man));
    CHECK(pelvis_w(man) > pelvis_w(elf));

    // A dwarf sprouts a hair-coloured beard (attachment bones the bald others lack).
    auto beard_bones = [](const CharacterModel& m) {
        int n = 0;
        for (const Bone& b : m.bones()) {
            if (b.attachment && b.color == BoneColor::Hair) {
                ++n;
            }
        }
        return n;
    };
    CHECK(beard_bones(dwarf) > 0);
    CHECK(beard_bones(man) == 0);
    CHECK(beard_bones(elf) == 0);

    // An elf has long ears: its tallest skin-coloured face feature far exceeds a man's.
    auto tallest_skin_feature = [](const CharacterModel& m) {
        f32 t = 0.0f;
        for (const Bone& b : m.bones()) {
            if (b.attachment && b.color == BoneColor::Skin) {
                t = std::max(t, b.box_size.y);
            }
        }
        return t;
    };
    CHECK(tallest_skin_feature(elf) > tallest_skin_feature(man) * 1.5f);
}

namespace {
// A dressed character the way the client builds one: model + outfit attachments, skinned body + outfit,
// the fitted cloth-collision body and the outfit's flowing cloth pieces.
struct Dressed {
    CharacterModel model;
    SkinnedMesh body, outfit;
    BodyColliders fit;
    std::vector<ClothPiece> cloth;
};
Dressed dress(u32 seed, const CharacterAppearance& app, OutfitKind kind, const Equipment& eq) {
    Dressed d{CharacterModel::create(seed, app), {}, {}, {}, {}};
    apply_outfit(d.model, kind, eq);
    d.body = build_body_mesh(d.model);
    d.outfit = build_outfit_mesh(d.model, kind, eq);
    d.fit = fit_body_colliders(d.model, d.body, d.outfit);
    d.cloth = outfit_cloth(d.model, kind, eq, d.fit);
    return d;
}
ClothPiece* find_cape(std::vector<ClothPiece>& cloth) {
    for (ClothPiece& c : cloth) {
        if (c.ring && !c.closed) {
            return &c;
        }
    }
    return nullptr;
}
// Deepest any (non-anchor) cloth node sits inside a collider it should rest on.
f32 deepest_penetration(const ClothPiece& c, const std::vector<ClothCollider>& body) {
    f32 worst = 0.0f;
    for (const ClothChain& ch : c.chains) {
        for (usize i = 1; i < ch.pos.size(); ++i) {
            for (const ClothCollider& k : body) {
                if ((k.group & c.collide) == 0) {
                    continue;
                }
                const Vec3 ab = k.b - k.a;
                const f32 l2 = glm::dot(ab, ab);
                const f32 t = l2 > 1e-10f ? glm::clamp(glm::dot(ch.pos[i] - k.a, ab) / l2, 0.0f, 1.0f) : 0.0f;
                worst = std::max(worst, k.r - glm::length(ch.pos[i] - (k.a + ab * t)));
            }
        }
    }
    return worst;
}
} // namespace

TEST_CASE("ClothRig: the cape collar anchors at the shoulders, not the waist") {
    // Cloaks hang from the Torso joint frame at the shoulder line (Character/ClothRig make_cape). The
    // collar must sit high on the body, not at mid-body (~the waist joint). Checked in a walk pose (the
    // in-game case), not just bind.
    Dressed d = dress(7u, CharacterAppearance{}, OutfitKind::Plate, Equipment{3, 3, 0, 0});
    const ClothPiece* cape = find_cape(d.cloth);
    REQUIRE(cape != nullptr);
    CharacterAnimator anim;
    for (int i = 0; i < 20; ++i) {
        anim.update(2.5f, Timestep{1.0f / 60.0f});
    }
    const std::vector<Mat4> jm = d.model.joint_matrices(Mat4{1.0f}, anim.pose(d.model));
    const int torso = d.model.bone_index(BonePart::Torso);
    REQUIRE(torso >= 0);
    REQUIRE(cape->anchor == BonePart::Torso);
    const f32 torso_y = jm[static_cast<usize>(torso)][3].y;
    for (const Vec3& a : cape->anchor_locals) {
        const Vec3 collar{jm[static_cast<usize>(torso)] * Vec4{a, 1.0f}};
        CHECK(collar.y > d.model.height() * 0.68f); // the collar is up at the shoulders / neck
        CHECK(collar.y > torso_y + 0.4f);           // and well above the mid-body (waist) joint
        CHECK(collar.z < 0.0f);                      // behind the body (the rig faces +Z)
    }
}

TEST_CASE("ClothRig: body colliders fit the worn gear") {
    const Dressed knight = dress(5u, CharacterAppearance{}, OutfitKind::Plate, Equipment{3, 3, 0, 0});
    // A bare-headed townsman (tint bits 2-3 = 3: no hood, whose shoulder capelet is broad itself).
    const Dressed peasant = dress(5u, CharacterAppearance{}, OutfitKind::Peasant, Equipment{0, 0, 12, 0});
    REQUIRE_FALSE(knight.fit.caps.empty());
    // Plate (+ pauldrons, gorget) is bulkier than a tunic, so the knight's collision body is broader.
    CHECK(knight.fit.torso_half_width > peasant.fit.torso_half_width);
    CHECK(knight.fit.caps.size() > peasant.fit.caps.size()); // the bulky gear adds capsules
    // Every skinned body vertex sits inside (or on) the fitted shape - the cloth rests ON the body.
    std::vector<ClothCollider> bind;
    pose_body_colliders(peasant.fit, peasant.model.joint_matrices(Mat4{1.0f}, {}), bind);
    usize outside = 0;
    for (const SkinVertex& v : peasant.body.vertices) {
        bool in = false;
        for (const ClothCollider& k : bind) {
            const Vec3 ab = k.b - k.a;
            const f32 l2 = glm::dot(ab, ab);
            const f32 t = l2 > 1e-10f ? glm::clamp(glm::dot(v.position - k.a, ab) / l2, 0.0f, 1.0f) : 0.0f;
            in = in || glm::length(v.position - (k.a + ab * t)) <= k.r + 1e-3f;
        }
        outside += in ? 0u : 1u;
    }
    CHECK(outside == 0u);
}

TEST_CASE("ClothRig: a cape drapes over the walking body without clipping into it") {
    Dressed d = dress(9u, CharacterAppearance{}, OutfitKind::Plate, Equipment{3, 3, 1, 0});
    REQUIRE(find_cape(d.cloth) != nullptr);
    CharacterAnimator anim;
    std::vector<ClothCollider> body;
    const f32 dt = 1.0f / 60.0f;
    f32 worst = 0.0f;
    f32 yaw = 0.0f;
    Vec3 at{0.0f};
    for (int f = 0; f < 240; ++f) {
        // Walk forward at a brisk pace, weaving (turning), so legs + arms swing into the cloth.
        anim.update(4.5f, Timestep{dt});
        yaw += std::sin(static_cast<f32>(f) * 0.05f) * 0.04f;
        at += Vec3{std::sin(yaw), 0.0f, std::cos(yaw)} * (4.5f * dt);
        const Mat4 root = glm::translate(Mat4{1.0f}, at) * glm::rotate(Mat4{1.0f}, yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                          anim.body_offset();
        const std::vector<Mat4> jm = d.model.joint_matrices(root, anim.pose(d.model));
        pose_body_colliders(d.fit, jm, body);
        ClothEnv env;
        env.wind = Vec3{2.0f, 0.0f, 0.5f};
        env.dt = dt;
        env.body = body;
        env.ground = 0.0f;
        for (ClothPiece& c : d.cloth) {
            step_cloth(c, d.model, jm, root, env);
            if (f > 30) {
                worst = std::max(worst, deepest_penetration(c, body));
            }
        }
    }
    // Every node ends each frame resting on (or off) the body surface - never sunk into it.
    CHECK(worst < 0.02f);
}

TEST_CASE("ClothRig: a cape whipped through the body by a sudden turn comes back out behind") {
    Dressed d = dress(3u, CharacterAppearance{}, OutfitKind::Holy, Equipment{3, 3, 2, 0});
    ClothPiece* cape = find_cape(d.cloth);
    REQUIRE(cape != nullptr);
    std::vector<ClothCollider> body;
    auto run = [&](f32 yaw, int frames) {
        const Mat4 root = glm::rotate(Mat4{1.0f}, yaw, Vec3{0.0f, 1.0f, 0.0f});
        const std::vector<Mat4> jm = d.model.joint_matrices(root, {});
        pose_body_colliders(d.fit, jm, body);
        ClothEnv env;
        env.body = body;
        env.ground = 0.0f;
        for (int f = 0; f < frames; ++f) {
            step_cloth(*cape, d.model, jm, root, env);
        }
    };
    run(0.0f, 60); // settle hanging down the back
    run(Pi, 1);    // snap round 180 degrees in one frame: the hanging cape is now in front of the body
    run(Pi, 90);
    // Facing -Z now (yaw = pi turns +Z to -Z), so "behind" is +Z: every node below the collar is there.
    for (const ClothChain& ch : cape->chains) {
        for (usize i = 2; i < ch.pos.size(); ++i) {
            CHECK(ch.pos[i].z > 0.05f);
        }
    }
}

TEST_CASE("BodyMesh: the action overlay deforms the skinned upper body (legs keep the walk)") {
    CharacterModel model = CharacterModel::create(11u, CharacterAppearance{});
    const SkinnedMesh body = build_body_mesh(model);

    // Two animators advanced to the SAME locomotion phase (53 frames); one also plays a swing. So the
    // legs match exactly and any difference in the skinned mesh is the upper-body action overlay alone.
    const Timestep dt{1.0f / 60.0f};
    CharacterAnimator walk_only, with_swing;
    for (int k = 0; k < 53; ++k) {
        walk_only.update(5.0f, dt);
    }
    for (int k = 0; k < 40; ++k) {
        with_swing.update(5.0f, dt);
    }
    with_swing.play_swing();
    for (int k = 0; k < 13; ++k) {
        with_swing.update(5.0f, dt); // ~mid-chop, now at frame 53 like walk_only
    }

    std::vector<Vertex> a, b;
    skin(body, model.joint_matrices(Mat4{1.0f}, walk_only.pose(model)), a);
    skin(body, model.joint_matrices(Mat4{1.0f}, with_swing.pose(model)), b);
    REQUIRE(a.size() == b.size());

    f32 max_shift = 0.0f, leg_shift = 0.0f;
    for (usize i = 0; i < a.size(); ++i) {
        const f32 d = glm::length(b[i].position - a[i].position);
        max_shift = std::max(max_shift, d);
        if (a[i].position.y < 0.4f) {
            leg_shift = std::max(leg_shift, d); // lower-leg / foot vertices
        }
    }
    CHECK(max_shift > 0.1f);  // the swing visibly deforms the skinned mesh (the arm sweeps up)
    CHECK(leg_shift < 0.02f); // the legs are untouched by the upper-body action (same walk phase)
}

TEST_CASE("CharacterAnimator: the attack swing is a diagonal cut from high right to low left") {
    CharacterModel model = CharacterModel::create(11u, CharacterAppearance{});
    const int arm = model.bone_index(BonePart::LowerArmL); // sword arm (rig labels mirrored: L = player's right)
    REQUIRE(arm >= 0);
    const f32 wrist = model.bones()[static_cast<usize>(arm)].box_center.y * 2.0f;

    // The sword-hand (wrist) position at `t` seconds into a fresh swing (idle underneath, so only the
    // swing moves the arm). The rig faces +Z with +Y up, so the player's right is -X.
    auto hand_at = [&](f32 t) {
        CharacterAnimator anim;
        anim.play_swing();
        const Timestep dt{1.0f / 240.0f};
        for (f32 e = 0.0f; e < t; e += dt.seconds) {
            anim.update(0.0f, dt);
        }
        const Mat4 m = model.joint_matrices(Mat4{1.0f}, anim.pose(model))[static_cast<usize>(arm)] *
                       glm::translate(Mat4{1.0f}, Vec3{0.0f, wrist, 0.0f});
        return Vec3{m[3]};
    };

    const Vec3 rest = hand_at(0.0f);
    const Vec3 windup = hand_at(0.14f); // raised high over the right shoulder
    const Vec3 impact = hand_at(0.27f); // cut down + across to the left, out in front

    CHECK(windup.y > rest.y + 0.4f);   // a real wind-up: the hand goes up above the shoulder
    CHECK(windup.x < 0.0f);            // ...on the player's right
    CHECK(impact.x > windup.x + 0.4f); // sweeps right -> left
    CHECK(impact.y < windup.y - 0.4f); // ...and down (a diagonal cut, not a flat sweep)
    CHECK(impact.z > rest.z + 0.2f);   // ...out in front of the body
}

TEST_CASE("ClothChain: hangs under gravity, blows in wind, and falls when detached") {
    const f32 dt = 1.0f / 60.0f;
    const Vec3 anchor{0.0f, 1.5f, 0.0f};
    ClothChain c;
    c.init(anchor, Vec3{0.0f, -1.0f, 0.0f}, 4, 0.12f, 0.2f);
    REQUIRE(c.pos.size() == 5);

    // Settle under gravity (no wind): it hangs straight down below the anchor.
    for (int k = 0; k < 180; ++k) {
        c.step(anchor, Vec3{0.0f}, 9.0f, dt);
    }
    CHECK(c.pos.front().y == doctest::Approx(anchor.y)); // node 0 stays pinned to the anchor
    CHECK(c.pos.back().y < anchor.y - 0.3f);             // the hem hangs well below
    CHECK(std::abs(c.pos.back().x) < 0.05f);             // roughly straight down

    // A steady wind along +x blows the hem downwind.
    for (int k = 0; k < 180; ++k) {
        c.step(anchor, Vec3{9.0f, 0.0f, 0.0f}, 9.0f, dt);
    }
    CHECK(c.pos.back().x > 0.08f);

    // Detached: node 0 is no longer pinned, so the whole chain free-falls.
    c.detach();
    CHECK_FALSE(c.attached);
    for (int k = 0; k < 90; ++k) {
        c.step(anchor, Vec3{0.0f}, 9.0f, dt);
    }
    CHECK(c.pos.front().y < anchor.y - 0.2f); // the anchor end has dropped

    // A detach with a sideways velocity kick (a cut / blow) drifts the piece horizontally as it falls.
    ClothChain k2;
    k2.init(anchor, Vec3{0.0f, -1.0f, 0.0f}, 4, 0.12f, 0.2f);
    for (int i = 0; i < 60; ++i) {
        k2.step(anchor, Vec3{0.0f}, 9.0f, dt); // settle
    }
    const f32 x0 = k2.pos.back().x;
    k2.detach();
    for (Vec3& pp : k2.prev) {
        pp -= Vec3{0.06f, 0.0f, 0.0f}; // a +x velocity kick (prev behind pos)
    }
    for (int i = 0; i < 30; ++i) {
        k2.step(anchor, Vec3{0.0f}, 9.0f, dt);
    }
    CHECK(k2.pos.back().x > x0 + 0.1f); // it flew off in +x, not straight down

    // The sheet mesh builds as valid double-sided geometry.
    MeshData md;
    build_cloth_mesh(c, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.4f, 0.3f, 0.6f}, md);
    CHECK(md.vertices.size() > 0);
    CHECK(md.indices.size() % 3 == 0);
}

TEST_CASE("Equipment: tiers grant a monotonic, buyable power bonus") {
    // Ragged (the starting gear) grants nothing.
    const Equipment ragged; // all defaults = tier 0
    const EquipBonus r = equipment_bonus(ragged);
    CHECK(r.health_add == doctest::Approx(0.0f));
    CHECK(r.mitigation_add == doctest::Approx(0.0f));
    CHECK(r.damage_mult == doctest::Approx(1.0f));
    CHECK(tier_price(EquipmentTier::Ragged) == 0u);

    // Each tier is a clear step up in survivability + damage.
    f32 last_hp = -1.0f, last_dmg = 0.0f;
    u32 last_price = 0;
    for (u8 t = 0; t < kTierCount; ++t) {
        Equipment e;
        e.outfit_tier = t;
        e.weapon_tier = t;
        const EquipBonus b = equipment_bonus(e);
        CHECK(b.health_add >= last_hp);
        CHECK(b.damage_mult >= last_dmg);
        CHECK(b.mitigation_add < 1.0f);
        last_hp = b.health_add;
        last_dmg = b.damage_mult;
        if (t > 0) {
            CHECK(tier_price(static_cast<EquipmentTier>(t)) > last_price); // dearer each tier
        }
        last_price = tier_price(static_cast<EquipmentTier>(t));
    }

    // Master gear is a big, distinct upgrade over ragged.
    Equipment master;
    master.outfit_tier = 3;
    master.weapon_tier = 3;
    const EquipBonus mb = equipment_bonus(master);
    CHECK(mb.health_add > 50.0f);
    CHECK(mb.damage_mult > 1.4f);
    CHECK(mb.mitigation_add > 0.0f);

    // Palette lookups wrap safely; tiers carry distinct accents (rags vs gold).
    CHECK(outfit_tints().size() == 8);
    CHECK(outfit_tint_of(200) == outfit_tints()[200 % 8]);
    CHECK(tier_accent(EquipmentTier::Master) != tier_accent(EquipmentTier::Ragged));
    CHECK(tier_sheen(EquipmentTier::Master) > tier_sheen(EquipmentTier::Ragged));
}

TEST_CASE("CharacterModel: deterministic generation and valid hierarchy") {
    const CharacterModel a = CharacterModel::generate(7);
    const CharacterModel b = CharacterModel::generate(7);

    // 13 core skeleton bones (parts 0..12) + joint fillers (neck/hands/ball joints) that connect them.
    REQUIRE(a.bone_count() >= 13);
    CHECK(b.bone_count() == a.bone_count());
    CHECK(a.bone_index(BonePart::Head) == 2); // head stays index 2 (face/hair features parent to it)
    CHECK(a.bone_index(BonePart::FootR) >= 0);
    CHECK(a.palette().skin == b.palette().skin);   // same seed => identical
    CHECK(a.palette().shirt == b.palette().shirt);
    CHECK(a.height() == doctest::Approx(b.height()));
    CHECK(a.height() > 1.3f); // stylised heroic proportions (~1.6 m, ~5.8 heads)
    CHECK(a.height() < 1.85f);

    // Parents always precede their children (single-pass transform safe).
    for (usize i = 0; i < a.bones().size(); ++i) {
        CHECK(a.bones()[i].parent < static_cast<int>(i));
    }

    // Different seeds produce different characters.
    const CharacterModel c = CharacterModel::generate(99);
    const bool different = a.height() != doctest::Approx(c.height()) ||
                           glm::length(a.palette().shirt - c.palette().shirt) > 0.01f;
    CHECK(different);
}

TEST_CASE("CharacterModel: create() applies appearance and adds feature bones") {
    CharacterAppearance look;
    look.skin = 3;
    look.hair_color = 4;
    look.eyes = EyeStyle::Wide;
    look.ears = EarStyle::Pointed;
    look.hair = HairStyle::Mohawk;

    const CharacterModel m = CharacterModel::create(7, look);

    // Body (13) + 2 eyes + 2 ears + hair bones.
    CHECK(m.bone_count() > 13);
    CHECK(m.palette().skin == skin_color(3));
    CHECK(m.palette().hair == hair_color_of(4));

    // The added features are parented to the head and keep the precede-children
    // invariant, and at least one eye/hair bone exists.
    int eyes = 0;
    int hair = 0;
    for (usize i = 0; i < m.bones().size(); ++i) {
        CHECK(m.bones()[i].parent < static_cast<int>(i));
        if (m.bones()[i].color == BoneColor::Eye) ++eyes;
        if (m.bones()[i].color == BoneColor::Hair) ++hair;
    }
    CHECK(eyes == 2);
    CHECK(hair >= 1); // at least one Mohawk bone

    // A bald character adds no hair bones (the simple face has no eyebrows).
    CharacterAppearance bald = look;
    bald.hair = HairStyle::Bald;
    const CharacterModel b = CharacterModel::create(7, bald);
    int bald_hair = 0;
    for (const Bone& bone : b.bones()) {
        if (bone.color == BoneColor::Hair) ++bald_hair;
    }
    CHECK(bald_hair == 0);
}

TEST_CASE("CharacterModel: bind pose stands upright (feet down, head up)") {
    const CharacterModel m = CharacterModel::generate(3);
    const std::vector<Quat> no_pose; // empty -> all identity
    const std::vector<Mat4> mats = m.bone_matrices(Mat4{1.0f}, no_pose);

    f32 head_y = -100.0f;
    f32 pelvis_y = 0.0f;
    f32 foot_y = 100.0f;
    for (usize i = 0; i < m.bones().size(); ++i) {
        const f32 y = mats[i][3].y;
        switch (m.bones()[i].part) {
            case BonePart::Head: head_y = y; break;
            case BonePart::Pelvis: pelvis_y = y; break;
            case BonePart::LowerLegL: foot_y = y; break;
            default: break;
        }
    }
    CHECK(head_y > pelvis_y);
    CHECK(pelvis_y > foot_y);
    CHECK(head_y > 0.85f); // big head still sits well above the feet
}

TEST_CASE("CharacterAnimator: walking advances the cycle; idle is neutral") {
    CharacterAnimator anim;
    for (int i = 0; i < 120; ++i) {
        anim.update(6.0f, Timestep{1.0f / 60.0f});
    }
    CHECK(anim.stride() > 0.6f);
    CHECK(anim.phase() > 0.0f);

    const CharacterModel m = CharacterModel::generate(2);
    const std::vector<Quat> walk = anim.pose(m);
    Quat left{1.0f, 0.0f, 0.0f, 0.0f};
    Quat right{1.0f, 0.0f, 0.0f, 0.0f};
    for (usize i = 0; i < m.bones().size(); ++i) {
        if (m.bones()[i].part == BonePart::UpperLegL) left = walk[i];
        if (m.bones()[i].part == BonePart::UpperLegR) right = walk[i];
    }
    // Legs swing out of phase: the L/R upper-leg X rotation components are opposite.
    CHECK(left.x == doctest::Approx(-right.x).epsilon(0.01));

    CharacterAnimator idle;
    for (int i = 0; i < 200; ++i) {
        idle.update(0.0f, Timestep{1.0f / 60.0f});
    }
    CHECK(idle.stride() < 0.05f);
    const std::vector<Quat> idle_pose = idle.pose(m);
    for (const Quat& q : idle_pose) {
        CHECK(q.w == doctest::Approx(1.0f).epsilon(0.02)); // identity-ish (no fore-aft swing)
        CHECK(std::abs(q.x) < 0.05f);
    }
}

TEST_CASE("CharacterAnimator: arms swing in circular arcs (not a flat pendulum)") {
    CharacterAnimator anim;
    for (int i = 0; i < 90; ++i) {
        anim.update(6.0f, Timestep{1.0f / 60.0f});
    }
    const CharacterModel m = CharacterModel::generate(2);
    const std::vector<Quat> walk = anim.pose(m);
    for (usize i = 0; i < m.bones().size(); ++i) {
        if (m.bones()[i].part == BonePart::UpperArmL) {
            // A purely fore-aft (X-axis) swing would have q.y == q.z == 0. The wobbly arms
            // add a sideways (Z) component + outward splay, so the hand traces a circle.
            CHECK(std::abs(walk[i].z) > 0.02f);
        }
    }
}

TEST_CASE("CharacterAnimator: body_offset bounces (squash/stretch) walking, calm idle") {
    // Idle: the body transform stays close to identity apart from a soft breathe.
    CharacterAnimator idle;
    for (int i = 0; i < 200; ++i) {
        idle.update(0.0f, Timestep{1.0f / 60.0f});
    }
    const Mat4 bi = idle.body_offset();
    CHECK(std::abs(bi[3].x) < 0.02f); // barely any sway
    CHECK(std::abs(bi[3].y) < 0.05f); // barely any bob
    CHECK(glm::length(Vec3{bi[1]}) == doctest::Approx(1.0f).epsilon(0.05)); // ~no squash

    // Walking: sample the vertical scale (column length = squash/stretch factor) over a
    // full cycle - it must visibly bounce.
    CharacterAnimator walk;
    for (int i = 0; i < 60; ++i) {
        walk.update(6.0f, Timestep{1.0f / 60.0f});
    }
    f32 min_sy = 1e9f;
    f32 max_sy = -1e9f;
    for (int i = 0; i < 120; ++i) {
        walk.update(6.0f, Timestep{1.0f / 60.0f});
        const f32 sy = glm::length(Vec3{walk.body_offset()[1]});
        min_sy = std::min(min_sy, sy);
        max_sy = std::max(max_sy, sy);
    }
    CHECK(max_sy - min_sy > 0.03f); // the jelly squash & stretch actually happens
}

TEST_CASE("CharacterModel: joint_matrices carry orientation (a held weapon follows the arm)") {
    const CharacterModel m = CharacterModel::generate(3);
    auto idx = [&](BonePart p) {
        for (usize i = 0; i < m.bones().size(); ++i) {
            if (m.bones()[i].part == p) return static_cast<int>(i);
        }
        return -1;
    };
    const int rh = idx(BonePart::LowerArmR);
    const int ru = idx(BonePart::UpperArmR);
    REQUIRE(rh >= 0);
    REQUIRE(ru >= 0);

    const std::vector<Quat> bind(m.bone_count(), QuatIdentity);
    const std::vector<Mat4> jb = m.joint_matrices(Mat4{1.0f}, bind);

    std::vector<Quat> pose(m.bone_count(), QuatIdentity);
    pose[static_cast<usize>(ru)] = glm::angleAxis(1.2f, Vec3{1.0f, 0.0f, 0.0f}); // raise the arm
    const std::vector<Mat4> jp = m.joint_matrices(Mat4{1.0f}, pose);

    // The forearm joint's world position moved with the raised upper arm...
    CHECK(glm::length(Vec3{jb[rh][3]} - Vec3{jp[rh][3]}) > 0.05f);
    // ...and crucially its ORIENTATION changed too (the bone's local +Y axis now points a
    // different way) - so a weapon attached to this frame rotates WITH the arm, not just slides.
    const Vec3 yb = glm::normalize(Vec3{jb[rh][1]});
    const Vec3 yp = glm::normalize(Vec3{jp[rh][1]});
    CHECK(glm::dot(yb, yp) < 0.99f);
}

TEST_CASE("CharacterAnimator: actions blend over locomotion (legs keep walking)") {
    const CharacterModel m = CharacterModel::generate(2);
    auto bone = [&](const std::vector<Quat>& pose, BonePart part) {
        for (usize i = 0; i < m.bones().size(); ++i) {
            if (m.bones()[i].part == part) return pose[i];
        }
        return QuatIdentity;
    };
    auto same = [](const Quat& a, const Quat& b) { return std::abs(glm::dot(a, b)) > 0.999f; };

    // Two animators stepped identically (same walk cycle); one also swings. The action must
    // only change the UPPER body - the legs must stay bit-for-bit in step with the plain walker.
    CharacterAnimator plain, acting;
    auto step_both = [&](int n) {
        for (int i = 0; i < n; ++i) {
            plain.update(6.0f, Timestep{1.0f / 60.0f});
            acting.update(6.0f, Timestep{1.0f / 60.0f});
        }
    };
    step_both(60);
    acting.play_swing();
    CHECK(acting.swinging());
    step_both(9); // ~0.15s into the swing
    {
        const std::vector<Quat> pw = plain.pose(m);
        const std::vector<Quat> ps = acting.pose(m);
        CHECK_FALSE(same(bone(pw, BonePart::UpperArmR), bone(ps, BonePart::UpperArmR))); // arm moved
        CHECK(same(bone(pw, BonePart::UpperLegL), bone(ps, BonePart::UpperLegL)));       // legs in step
        CHECK(same(bone(pw, BonePart::UpperLegR), bone(ps, BonePart::UpperLegR)));
    }
    // The swing is one-shot: it ends and the arm rejoins locomotion.
    step_both(60);
    CHECK_FALSE(acting.swinging());

    // Blocking raises the off (left) arm and holds it; the legs still match the plain walker.
    acting.set_blocking(true);
    step_both(30);
    CHECK(acting.blocking());
    {
        const std::vector<Quat> pw = plain.pose(m);
        const std::vector<Quat> pb = acting.pose(m);
        CHECK_FALSE(same(bone(pw, BonePart::UpperArmL), bone(pb, BonePart::UpperArmL)));
        CHECK(same(bone(pw, BonePart::UpperLegR), bone(pb, BonePart::UpperLegR)));
    }

    // Casting thrusts the weapon arm forward (a one-shot) while the legs keep walking.
    acting.set_blocking(false);
    step_both(30); // let the block ease out
    acting.play_cast();
    CHECK(acting.casting());
    step_both(14); // ~mid-cast
    {
        const std::vector<Quat> pw = plain.pose(m);
        const std::vector<Quat> pc = acting.pose(m);
        CHECK_FALSE(same(bone(pw, BonePart::UpperArmL), bone(pc, BonePart::UpperArmL))); // weapon arm thrusts
        CHECK(same(bone(pw, BonePart::UpperLegL), bone(pc, BonePart::UpperLegL)));        // legs in step
        CHECK(same(bone(pw, BonePart::UpperLegR), bone(pc, BonePart::UpperLegR)));
    }
    step_both(50);
    CHECK_FALSE(acting.casting()); // one-shot ends, the arm rejoins locomotion
}

// The sword swing is authored as a hand path + blade direction and solved with two-bone IK + a wrist
// turn: check the solved rig really puts the hand where it's keyed and points the blade the keyed way.
TEST_CASE("Character: the sword swing's IK lands the hand + blade where they're keyed") {
    CharacterModel model = CharacterModel::create(60u, CharacterAppearance{});
    CharacterAnimator anim;
    anim.update(0.0f, Timestep{1.0f / 60.0f});
    anim.play_swing();
    const int iu = model.bone_index(BonePart::UpperArmL);
    const int il = model.bone_index(BonePart::LowerArmL);
    const int it = model.bone_index(BonePart::Torso);
    REQUIRE(iu >= 0);
    REQUIRE(il >= 0);
    REQUIRE(it >= 0);
    const f32 l1 = std::abs(model.bones()[static_cast<usize>(il)].joint_offset.y);
    const f32 l2 = std::abs(model.bones()[static_cast<usize>(il)].box_center.y * 2.0f);
    for (int step = 0; step < 30; ++step) {
        anim.update(0.0f, Timestep{1.0f / 60.0f});
        if (anim.swing_env() < 0.99f) {
            continue; // only judge the fully-blended part of the swing
        }
        const CharacterAnimator::SwingParams p = anim.swing_params();
        const std::vector<Quat> pose = anim.pose(model);
        const std::vector<Mat4> jm = model.joint_matrices(Mat4{1.0f}, pose);
        const Mat4& torso = jm[static_cast<usize>(it)];
        const Mat4 hand = jm[static_cast<usize>(il)] * glm::translate(Mat4{1.0f}, Vec3{0.0f, -l2, 0.0f});
        // Hand position: the shoulder (in the torso's frame) + the keyed offset * arm length.
        const Vec3 shoulder = Vec3{torso * Vec4{model.bones()[static_cast<usize>(iu)].joint_offset, 1.0f}};
        const Vec3 want_hand = shoulder + Mat3{torso} * (p.hand * (l1 + l2));
        CAPTURE(step);
        CHECK(glm::length(Vec3{hand[3]} - want_hand) < 0.03f);
        // Blade direction.
        const Vec3 blade = glm::normalize(Mat3{hand * glm::mat4_cast(anim.weapon_wrist(model))} *
                                          CharacterAnimator::blade_axis());
        const Vec3 want_blade = glm::normalize(Mat3{torso} * p.blade);
        CHECK(glm::dot(blade, want_blade) > 0.97f);
    }
}
