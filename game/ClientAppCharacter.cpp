// ClientApp - posed character + held-weapon rendering.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

namespace alryn::game {

void ClientApp::draw_rig(const CharacterModel& model, const std::vector<Mat4>& mats, const Vec3& tint,
                         bool attachments_only) {
    const std::vector<Bone>& bones = model.bones();
    const CharacterPalette& pal = model.palette();
    for (usize i = 0; i < bones.size(); ++i) {
        if (attachments_only && !bones[i].attachment) {
            continue; // the skinned body already covers the core body + joint fillers
        }
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

const Mesh& ClientApp::shape_mesh(BoneShape s) const {
    return s == BoneShape::Sphere       ? shape_sphere_
           : s == BoneShape::Cylinder   ? shape_cylinder_
           : s == BoneShape::Capsule    ? shape_capsule_
           : s == BoneShape::RoundedBox ? shape_rounded_
                                        : shape_box_;
}

void ClientApp::draw_weapon(WeaponType type, const Mat4& hand, const CharacterPalette& pal,
                            EquipmentTier tier) {
    for (const WeaponPiece& wp : weapon_pieces(type, tier, pal)) {
        if (wp.emissive) {
            renderer_->draw_emissive(shape_mesh(wp.shape), hand * wp.local, Vec4{wp.color, 1.0f});
            renderer_->draw_glow(shape_mesh(wp.shape), hand * wp.local * glm::scale(Mat4{1.0f}, Vec3{1.7f}),
                                 Vec4{wp.color, 0.4f}); // soft halo around glowing pieces
        } else {
            renderer_->draw(shape_mesh(wp.shape), hand * wp.local, Vec4{wp.color, 1.0f});
        }
    }
}

void ClientApp::draw_role_weapon(const CharacterModel& model, const std::vector<Mat4>& jmats,
                                 PlayerRole role, const Equipment& eq) {
    // Modular weapons: the role's main-hand weapon (the player's chosen weapon_index) on the
    // L-suffixed arm (the player's right) and the off-hand (shield / dagger) on the R-suffixed arm,
    // both built from the shared weapon_pieces at the equipment's tier + palette.
    const CharacterPalette& pal = model.palette();
    const u8 r = static_cast<u8>(role);
    const EquipmentTier wt = eq.weapon();
    draw_weapon(role_weapon(r, eq.weapon_index), hand_frame(model, jmats, BonePart::LowerArmL), pal, wt);
    const WeaponType off = role_offhand(r);
    if (off != WeaponType::None) {
        draw_weapon(off, hand_frame(model, jmats, BonePart::LowerArmR), pal, wt);
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

void ClientApp::apply_idle_stance(const CharacterModel& model, std::vector<Quat>& pose, PlayerRole role,
                                  f32 weight) const {
    auto set = [&](BonePart p, const Quat& q) {
        const int i = model.bone_index(p);
        if (i >= 0 && static_cast<usize>(i) < pose.size()) {
            // Ease from the locomotion pose into the rest stance so stopping/starting doesn't snap.
            pose[static_cast<usize>(i)] = glm::slerp(pose[static_cast<usize>(i)], q, weight);
        }
    };
    const Vec3 X{1.0f, 0.0f, 0.0f};
    const Vec3 Z{0.0f, 0.0f, 1.0f};
    // The weapon hand is the L-suffixed arm (the rig's labels are mirrored). -X about the shoulder
    // brings the upper arm FORWARD/down; a bent elbow brings the hand in front.
    if (role == PlayerRole::Mage || role == PlayerRole::Cleric) {
        // Both hands rest on the weapon planted in front like a walking stick.
        set(BonePart::UpperArmL, glm::angleAxis(-0.62f, X) * glm::angleAxis(-0.18f, Z));
        set(BonePart::LowerArmL, glm::angleAxis(0.95f, X));
    } else if (role == PlayerRole::Hunter) {
        // Bow held lowered + a touch forward at the side.
        set(BonePart::UpperArmL, glm::angleAxis(-0.32f, X));
        set(BonePart::LowerArmL, glm::angleAxis(0.28f, X));
    }
}

void ClientApp::draw_planted_weapon(const CharacterModel& model, const std::vector<Mat4>& jmats,
                                    const Vec3& feet, PlayerRole role, const Equipment& eq) {
    const CharacterPalette& pal = model.palette();
    const Mat4 hand = hand_frame(model, jmats, BonePart::LowerArmL);
    const Vec3 grip = Vec3{hand[3]};                       // where the hand grips the shaft
    const Vec3 bottom{grip.x, feet.y, grip.z};             // stood straight on the ground
    const Vec3 top = grip + Vec3{0.0f, 0.5f, 0.0f};        // a TALL shaft rising past the hand
    const f32 len = std::max(0.6f, top.y - bottom.y);
    const Vec3 mid = (top + bottom) * 0.5f;
    renderer_->draw(shape_box_,
                    glm::translate(Mat4{1.0f}, mid) * orient_to(bottom - top) *
                        glm::scale(Mat4{1.0f}, Vec3{0.045f, 0.045f, len}),
                    Vec4{0.42f, 0.29f, 0.16f, 1.0f}); // wooden shaft
    if (role == PlayerRole::Mage) {
        // a glowing arcane orb crowning the staff (up near the head)
        renderer_->draw_emissive(shape_sphere_,
                                 glm::translate(Mat4{1.0f}, top) * glm::scale(Mat4{1.0f}, Vec3{0.15f}),
                                 Vec4{pal.glow * 1.7f, 1.0f});
        renderer_->draw_glow(shape_sphere_,
                             glm::translate(Mat4{1.0f}, top) * glm::scale(Mat4{1.0f}, Vec3{0.32f}),
                             Vec4{pal.glow, 0.4f});
    } else { // Cleric - a holy head crowning the shaft
        renderer_->draw(shape_sphere_,
                        glm::translate(Mat4{1.0f}, top) * glm::scale(Mat4{1.0f}, Vec3{0.13f}),
                        Vec4{pal.accent, 1.0f});
    }
    // Keep the off-hand (the Cleric's shield) in hand.
    const WeaponType off = role_offhand(static_cast<u8>(role));
    if (off != WeaponType::None) {
        draw_weapon(off, hand_frame(model, jmats, BonePart::LowerArmR), pal, eq.weapon());
    }
}

void ClientApp::skin_and_draw(const CharacterModel& model, const SkinnedMesh& src, Mesh& gpu,
                              const Mat4& root, const std::vector<Quat>& pose, const Vec3& tint) {
    if (src.vertices.empty()) {
        return;
    }
    // Distance cull: don't pay to CPU-skin + upload a character that's far outside the view. Generous
    // (cam-distance based) so anything on-screen always skins; this only spares far-flung town NPCs.
    if (glm::distance(Vec3{root[3]}, camera_.position()) > character::skin_cull_dist) {
        return;
    }
    const CharacterPalette& pal = model.palette();
    auto palette = [&](u8 mat) -> Vec3 { return body_material_color(pal, static_cast<BodyMaterial>(mat)); };
    // Skin in LOCAL space (pose only, no root) so the mesh's bind-pose bounding sphere * root gives a
    // correct cull sphere; `root` (translate+rotate+wobble) places it in the world as the model matrix.
    skin(src, model.joint_matrices(Mat4{1.0f}, pose), skin_scratch_, palette);
    if (!gpu.valid()) {
        MeshData md;
        md.vertices = skin_scratch_;
        md.indices = src.indices;
        gpu.create(renderer_->device(), md);
    } else {
        gpu.update_vertices(skin_scratch_);
    }
    renderer_->draw(gpu, root, Vec4{tint, 1.0f});
}

void ClientApp::retire_mesh(Mesh&& m) {
    if (m.valid()) {
        mesh_graveyard_.emplace_back(3, std::move(m)); // outlive the frames-in-flight that may use it
    }
}

void ClientApp::tick_mesh_graveyard() {
    for (auto it = mesh_graveyard_.begin(); it != mesh_graveyard_.end();) {
        if (--it->first <= 0) {
            it->second.destroy();
            it = mesh_graveyard_.erase(it);
        } else {
            ++it;
        }
    }
}

void ClientApp::draw_skinned_body(PlayerVisual& v, const Mat4& root, const std::vector<Quat>& pose,
                                  const Vec3& tint) {
    if (v.body_skin.vertices.empty()) {
        v.body_skin = build_body_mesh(v.model); // safety net if a visual was created without it
    }
    skin_and_draw(v.model, v.body_skin, v.body_mesh, root, pose, tint);     // the bare body underneath
    skin_and_draw(v.model, v.outfit_skin, v.outfit_mesh, root, pose, tint); // the worn armour / cloth
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
    std::vector<Quat> pose =
        seated ? CharacterAnimator::sit_pose(v.model) : v.animator.pose(v.model);
    // Standing still + not mid-action: strike a characterful idle stance instead of arms-straight-down
    // (staff/mace users rest on their planted weapon; the Hunter lowers the bow).
    const PlayerRole r = static_cast<PlayerRole>((role < 0 ? 0 : role) % kRoleCount);
    f32 idle_w = 0.0f; // 1 = fully standing/resting, 0 = moving or acting
    if (!seated && role >= 0 && !v.animator.swinging() && !v.animator.casting() &&
        !v.animator.blocking()) {
        idle_w = glm::smoothstep(0.55f, 0.15f, v.speed); // eases in as the player slows to a stop
    }
    if (idle_w > 0.01f) {
        apply_idle_stance(v.model, pose, r, idle_w);
    }
    // The continuous skinned body, then the face/hair/gear attachment primitives on top.
    draw_skinned_body(v, root, pose);
    const std::vector<Mat4> mats = v.model.bone_matrices(root, pose);
    draw_rig(v.model, mats, Vec3{1.0f}, /*attachments_only=*/true);
    if (role >= 0) {
        // Weapons attach to the JOINT frames (orientation + position) so they swing with
        // the arm, unlike the box mats whose columns are scaled by box_size.
        const std::vector<Mat4> jmats = v.model.joint_matrices(root, pose);
        draw_cloth(v, root, jmats, Vec3{1.0f}); // simulated flowing cloth (cape, ...)
        const bool staff_user = (r == PlayerRole::Mage || r == PlayerRole::Cleric);
        if (idle_w > 0.5f && staff_user) {
            draw_planted_weapon(v.model, jmats, feet, r, v.equipment); // rest on the planted staff/mace
        } else {
            draw_role_weapon(v.model, jmats, r, v.equipment);
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
