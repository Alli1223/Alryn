#include <doctest/doctest.h>

#include <Alryn/Character/BodyMesh.h>
#include <Alryn/Character/CharacterAnimator.h>
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
    CHECK(a.height() > 1.2f); // cute chibi proportions (~1.45 m, big head + short limbs)
    CHECK(a.height() < 1.7f);

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
    CHECK(hair >= 3); // 2 eyebrows (always) + at least one Mohawk bone

    // A bald character adds no HAIRSTYLE bones - only the 2 always-present eyebrows remain.
    CharacterAppearance bald = look;
    bald.hair = HairStyle::Bald;
    const CharacterModel b = CharacterModel::create(7, bald);
    int bald_hair = 0;
    for (const Bone& bone : b.bones()) {
        if (bone.color == BoneColor::Hair) ++bald_hair;
    }
    CHECK(bald_hair == 2);   // the eyebrows
    CHECK(hair > bald_hair); // the styled hair adds crown bones on top of the eyebrows
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
