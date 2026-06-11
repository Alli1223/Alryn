#include <doctest/doctest.h>

#include <Alryn/Character/CharacterAnimator.h>
#include <Alryn/Character/CharacterModel.h>

#include <cmath>

using namespace alryn;

TEST_CASE("CharacterModel: deterministic generation and valid hierarchy") {
    const CharacterModel a = CharacterModel::generate(7);
    const CharacterModel b = CharacterModel::generate(7);

    REQUIRE(a.bone_count() == 13);
    CHECK(b.bone_count() == 13);
    CHECK(a.palette().skin == b.palette().skin);   // same seed => identical
    CHECK(a.palette().shirt == b.palette().shirt);
    CHECK(a.height() == doctest::Approx(b.height()));

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
    CHECK(hair >= 1);

    // A bald character adds no hair bones.
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
        CHECK(q.w == doctest::Approx(1.0f).epsilon(0.02)); // identity-ish
        CHECK(std::abs(q.x) < 0.05f);
    }
}
