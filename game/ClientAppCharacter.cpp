// ClientApp - posed character + held-weapon rendering.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

namespace alryn::game {

void ClientApp::draw_rig(const CharacterModel& model, const std::vector<Mat4>& mats, const Vec3& tint) {
    const std::vector<Bone>& bones = model.bones();
    const CharacterPalette& pal = model.palette();
    for (usize i = 0; i < bones.size(); ++i) {
        Vec3 color{1.0f};
        bool glow = false;
        switch (bones[i].color) {
            case BoneColor::Skin: color = pal.skin; break;
            case BoneColor::Shirt: color = pal.shirt; break;
            case BoneColor::Pants: color = pal.pants; break;
            case BoneColor::Hair: color = pal.hair; break;
            case BoneColor::Eye: color = pal.eye; break;
            case BoneColor::Primary: color = pal.primary; break;
            case BoneColor::Accent: color = pal.accent; break;
            case BoneColor::Metal: color = pal.metal; break;
            case BoneColor::Dark: color = pal.dark; break;
            case BoneColor::Glow: color = pal.glow; glow = true; break;
        }
        const Mesh& shape = bones[i].shape == BoneShape::Sphere       ? shape_sphere_
                            : bones[i].shape == BoneShape::Cylinder   ? shape_cylinder_
                            : bones[i].shape == BoneShape::Capsule    ? shape_capsule_
                            : bones[i].shape == BoneShape::RoundedBox ? shape_rounded_
                                                                      : shape_box_;
        if (glow) {
            renderer_->draw_emissive(shape, mats[i], Vec4{color, 1.0f}); // lit eye-slit / arcane eyes
        } else {
            renderer_->draw(shape, mats[i], Vec4{color * tint, 1.0f});
        }
    }
}

void ClientApp::draw_held_spear(const CharacterModel& model, const std::vector<Mat4>& mats) {
    const std::vector<Bone>& bones = model.bones();
    int hand = -1;
    for (usize i = 0; i < bones.size(); ++i) {
        if (bones[i].part == BonePart::LowerArmR) {
            hand = static_cast<int>(i);
            break;
        }
    }
    if (hand < 0) {
        return;
    }
    const Vec3 grip = Vec3{mats[hand][3]}; // world position of the forearm/hand
    const Mat4 shaft = glm::translate(Mat4{1.0f}, grip + Vec3{0.0f, 0.45f, 0.0f}) *
                       glm::scale(Mat4{1.0f}, Vec3{0.055f, 1.9f, 0.055f});
    renderer_->draw(shape_box_, shaft, Vec4{0.4f, 0.28f, 0.16f, 1.0f}); // wooden haft
    const Mat4 head = glm::translate(Mat4{1.0f}, grip + Vec3{0.0f, 1.45f, 0.0f}) *
                      glm::scale(Mat4{1.0f}, Vec3{0.1f, 0.34f, 0.05f});
    renderer_->draw(shape_box_, head, Vec4{0.7f, 0.73f, 0.8f, 1.0f}); // steel spearhead
}

Mat4 ClientApp::hand_frame(const CharacterModel& model, const std::vector<Mat4>& jmats, BonePart arm) {
    const std::vector<Bone>& bones = model.bones();
    for (usize i = 0; i < bones.size(); ++i) {
        if (bones[i].part == arm) {
            const f32 wrist = bones[i].box_center.y * 2.0f; // far end of the forearm (local -Y)
            return jmats[i] * glm::translate(Mat4{1.0f}, Vec3{0.0f, wrist, 0.0f});
        }
    }
    return Mat4{1.0f};
}

void ClientApp::draw_role_weapon(const CharacterModel& model, const std::vector<Mat4>& jmats, PlayerRole role) {
    const Mat4 weapon_hand = hand_frame(model, jmats, BonePart::LowerArmL); // player's right
    switch (role) {
        case PlayerRole::Knight: {
            // Sword: blade continues along the forearm (local -Y), tilted slightly forward.
            const Mat4 grip = weapon_hand * glm::rotate(Mat4{1.0f}, -0.35f, Vec3{1.0f, 0.0f, 0.0f});
            renderer_->draw(shape_box_,
                            grip * glm::scale(Mat4{1.0f}, Vec3{0.30f, 0.05f, 0.07f}),
                            Vec4{0.36f, 0.26f, 0.12f, 1.0f}); // crossguard
            renderer_->draw(shape_box_,
                            grip * glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.12f, 0.0f}) *
                                glm::scale(Mat4{1.0f}, Vec3{0.05f, 0.22f, 0.05f}),
                            Vec4{0.3f, 0.2f, 0.1f, 1.0f}); // grip handle (into the fist)
            renderer_->draw(shape_box_,
                            grip * glm::translate(Mat4{1.0f}, Vec3{0.0f, -0.62f, 0.0f}) *
                                glm::scale(Mat4{1.0f}, Vec3{0.07f, 1.1f, 0.16f}),
                            Vec4{0.80f, 0.84f, 0.92f, 1.0f}); // blade
            // Shield on the off (player's left) hand, in FRONT of the forearm (local +Z).
            const Mat4 shield_hand = hand_frame(model, jmats, BonePart::LowerArmR);
            renderer_->draw(shape_rounded_,
                            shield_hand * glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.0f, 0.18f}) *
                                glm::scale(Mat4{1.0f}, Vec3{0.58f, 0.74f, 0.13f}),
                            Vec4{0.46f, 0.33f, 0.2f, 1.0f}); // shield
            renderer_->draw(shape_sphere_,
                            shield_hand * glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.0f, 0.25f}) *
                                glm::scale(Mat4{1.0f}, Vec3{0.13f}),
                            Vec4{0.82f, 0.7f, 0.32f, 1.0f}); // boss
            break;
        }
        case PlayerRole::Hunter: {
            // A vertical bow held in the hand: stave along the forearm axis.
            renderer_->draw(shape_box_,
                            weapon_hand * glm::scale(Mat4{1.0f}, Vec3{0.05f, 1.5f, 0.06f}),
                            Vec4{0.4f, 0.26f, 0.12f, 1.0f}); // stave
            for (f32 s : {1.0f, -1.0f}) {
                renderer_->draw(shape_box_,
                                weapon_hand *
                                    glm::translate(Mat4{1.0f}, Vec3{0.07f, s * 0.66f, 0.0f}) *
                                    glm::scale(Mat4{1.0f}, Vec3{0.05f, 0.22f, 0.05f}),
                                Vec4{0.4f, 0.26f, 0.12f, 1.0f}); // recurved tips
            }
            break;
        }
        case PlayerRole::Cleric:
            break; // staff drawn by draw_cleric_staff (walking-stick behaviour)
        case PlayerRole::Mage: {
            // A wizard's staff along the forearm, topped with a glowing arcane orb.
            renderer_->draw(shape_box_,
                            weapon_hand * glm::translate(Mat4{1.0f}, Vec3{0.0f, -0.35f, 0.0f}) *
                                glm::scale(Mat4{1.0f}, Vec3{0.055f, 1.5f, 0.055f}),
                            Vec4{0.32f, 0.22f, 0.14f, 1.0f}); // shaft
            const Mat4 orb = weapon_hand * glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.46f, 0.0f});
            renderer_->draw_emissive(shape_sphere_, orb * glm::scale(Mat4{1.0f}, Vec3{0.13f}),
                                     Vec4{0.7f, 0.5f, 1.0f, 1.0f}); // orb
            renderer_->draw_glow(shape_sphere_, orb * glm::scale(Mat4{1.0f}, Vec3{0.28f}),
                                 Vec4{0.6f, 0.42f, 1.0f, 0.5f});
            break;
        }
    }
}

void ClientApp::draw_cleric_staff(const CharacterModel& model, const std::vector<Mat4>& jmats, const Vec3& feet, const CharacterAnimator& anim, f32 yaw) {
    const Mat4 hand = hand_frame(model, jmats, BonePart::LowerArmL);
    const Vec3 top = Vec3{hand[3]};
    const Vec3 facing{std::cos(yaw), 0.0f, std::sin(yaw)};
    const f32 stride = anim.stride();
    const f32 ph = anim.phase();
    const f32 swing = std::cos(ph) * 0.42f * stride;             // tip: +ahead .. -behind
    const f32 reach = std::max(0.6f, top.y - feet.y + 0.05f);    // length down to the ground
    const Vec3 dir = glm::normalize(Vec3{facing.x * swing, -1.0f, facing.z * swing});
    const Vec3 bottom = top + dir * reach;
    const Vec3 mid = (top + bottom) * 0.5f;
    renderer_->draw(shape_box_,
                    glm::translate(Mat4{1.0f}, mid) * orient_to(bottom - top) *
                        glm::scale(Mat4{1.0f}, Vec3{0.05f, 0.05f, reach}),
                    Vec4{0.46f, 0.31f, 0.16f, 1.0f}); // shaft
    renderer_->draw_emissive(shape_sphere_,
                             glm::translate(Mat4{1.0f}, top + Vec3{0.0f, 0.16f, 0.0f}) *
                                 glm::scale(Mat4{1.0f}, Vec3{0.18f}),
                             Vec4{0.65f, 0.55f, 1.0f, 1.0f}); // glowing orb at the head
}

void ClientApp::draw_character(PlayerVisual& v, const Vec3& feet, f32 yaw, bool seated, int role) {
    // Seated riders sink onto the bench (the sit pose folds the legs forward).
    const Vec3 base = seated ? feet - Vec3{0.0f, 0.42f, 0.0f} : feet;
    Mat4 root = glm::translate(Mat4{1.0f}, base) *
                glm::rotate(Mat4{1.0f}, HalfPi - yaw, Vec3{0.0f, 1.0f, 0.0f});
    // The blobby squash/sway/lean wobble rides on the root when on foot (not seated).
    if (!seated) {
        root = root * v.animator.body_offset();
    }
    const std::vector<Quat> pose =
        seated ? CharacterAnimator::sit_pose(v.model) : v.animator.pose(v.model);
    const std::vector<Mat4> mats = v.model.bone_matrices(root, pose);
    draw_rig(v.model, mats);
    if (role >= 0) {
        // Weapons attach to the JOINT frames (orientation + position) so they swing with
        // the arm, unlike the box mats whose columns are scaled by box_size.
        const std::vector<Mat4> jmats = v.model.joint_matrices(root, pose);
        const PlayerRole r = static_cast<PlayerRole>(role % kRoleCount);
        if (r == PlayerRole::Cleric) {
            draw_cleric_staff(v.model, jmats, feet, v.animator, yaw);
        } else {
            draw_role_weapon(v.model, jmats, r);
        }
        // A steel motion trail off the real blade tip while a Knight is mid-swing (the sword
        // is on the player's right = the L-suffixed bone).
        if (r == PlayerRole::Knight && v.animator.swinging()) {
            const Mat4 grip = hand_frame(v.model, jmats, BonePart::LowerArmL) *
                              glm::rotate(Mat4{1.0f}, -0.35f, Vec3{1.0f, 0.0f, 0.0f});
            const Vec3 tip = Vec3{(grip * glm::translate(Mat4{1.0f}, Vec3{0.0f, -1.15f, 0.0f}))[3]};
            emit(tip, Vec3{0.0f}, Vec4{0.92f, 0.96f, 1.0f, 0.8f}, 0.16f, 0.17f, 1);
        }
    }
}

} // namespace alryn::game
