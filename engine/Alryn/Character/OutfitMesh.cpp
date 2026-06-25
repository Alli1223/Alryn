#include <Alryn/Character/OutfitMesh.h>

#include <Alryn/Character/SkinBuilder.h>

namespace alryn {

using namespace skinbuild;

namespace {

// Shared bind-pose accessors for a character skeleton (the joint frames + per-part metrics the body
// mesh is built from), so the outfit overlays the SAME limbs with the SAME bone weights and bends in
// lockstep with the body when posed.
struct Rig {
    const CharacterModel& model;
    std::vector<Mat4> J; // bind joint frames

    explicit Rig(const CharacterModel& m) : model(m), J(m.joint_matrices(Mat4{1.0f}, {})) {}

    int bi(BonePart p) const { return model.bone_index(p); }
    Vec3 jp(BonePart p) const {
        const int i = bi(p);
        return i < 0 ? Vec3{0.0f} : Vec3{J[static_cast<usize>(i)][3]};
    }
    f32 seg(BonePart p) const {
        const int i = bi(p);
        return i < 0 ? 0.3f : -2.0f * model.bones()[static_cast<usize>(i)].box_center.y;
    }
    f32 rad(BonePart p) const {
        const int i = bi(p);
        return i < 0 ? 0.08f : model.bones()[static_cast<usize>(i)].box_size.x * 0.5f;
    }
};

// A clad limb: a tube over the body arm/leg, weighted across the shoulder/elbow (or hip/knee) exactly
// like the body so it bends with the joint. `coverage` (0..1) is how far down the lower segment the
// cladding reaches (1 = to the wrist/ankle); `rscale` inflates it just outside the bare limb.
void clad_limb(SkinnedMesh& sm, const Rig& r, BonePart up, BonePart lo, f32 rscale, f32 coverage,
               BodyMaterial mat, bool root_to_torso) {
    const int iU = r.bi(up), iL = r.bi(lo);
    const int iRoot = root_to_torso ? r.bi(BonePart::Torso) : r.bi(BonePart::Pelvis);
    if (iU < 0 || iL < 0) {
        return;
    }
    const Vec3 a = r.jp(up), b = r.jp(lo);
    const Vec3 end = b + glm::normalize(b - a) * (r.seg(lo) * coverage);
    const f32 rr = r.rad(up) * rscale;
    tube(sm,
         {a, glm::mix(a, b, 0.5f), b, glm::mix(b, end, 0.6f), end},
         {rr * 1.12f, rr * 0.98f, rr * 0.94f, rr * 0.9f, rr * 0.85f},
         {{{iRoot, 0.4f}, {iU, 0.6f}}, {{iU, 1.0f}}, {{iU, 0.5f}, {iL, 0.5f}}, {{iL, 1.0f}}, {{iL, 1.0f}}},
         mat, false, true);
}

// A torso shell: a tube over the chest weighted pelvis -> torso -> neck, squashed in z so it reads as a
// chest plate / robe body rather than a barrel.
void clad_torso(SkinnedMesh& sm, const Rig& r, f32 rscale, f32 squashz, BodyMaterial mat) {
    const int iP = r.bi(BonePart::Pelvis), iT = r.bi(BonePart::Torso), iH = r.bi(BonePart::Head);
    if (iP < 0 || iT < 0 || iH < 0) {
        return;
    }
    const Vec3 pelvis = r.jp(BonePart::Pelvis), neck = r.jp(BonePart::Head);
    const f32 tw = 0.22f * rscale;
    tube(sm,
         {pelvis - Vec3{0, 0.02f, 0}, glm::mix(pelvis, neck, 0.36f), glm::mix(pelvis, neck, 0.72f), neck},
         {tw * 1.04f, tw * 1.12f, tw, tw * 0.84f},
         {{{iP, 1.0f}}, {{iP, 0.5f}, {iT, 0.5f}}, {{iT, 1.0f}}, {{iT, 0.82f}, {iH, 0.18f}}},
         mat, true, false, squashz);
}

// A draping skirt hanging from the pelvis over both legs (one continuous z-flattened cone), down to
// `length_frac` of the lower leg - weighted to the pelvis so it hangs naturally.
void skirt(SkinnedMesh& sm, const Rig& r, f32 length_frac, f32 flare, BodyMaterial mat) {
    const int iP = r.bi(BonePart::Pelvis);
    if (iP < 0) {
        return;
    }
    const Vec3 hip = r.jp(BonePart::Pelvis);
    const f32 lu = r.seg(BonePart::LowerLegL), ll = r.seg(BonePart::FootL);
    const f32 drop = lu + ll * length_frac;
    const Vec3 top = hip + Vec3{0.0f, 0.12f, 0.0f};
    const Vec3 knee = hip - Vec3{0.0f, lu, 0.0f};
    const Vec3 hem = hip - Vec3{0.0f, drop, 0.0f};
    tube(sm, {top, glm::mix(top, knee, 0.6f), knee, hem},
         {flare * 0.62f, flare * 0.74f, flare * 0.9f, flare},
         {{{iP, 1.0f}}, {{iP, 1.0f}}, {{iP, 1.0f}}, {{iP, 1.0f}}}, mat, true, true, 0.7f);
}

} // namespace

SkinnedMesh build_outfit_mesh(const CharacterModel& model, OutfitKind kind, const Equipment& equip) {
    SkinnedMesh sm;
    if (model.bone_count() < 13) {
        return sm;
    }
    const Rig r(model);
    sm.inverse_bind.resize(r.J.size());
    for (usize b = 0; b < r.J.size(); ++b) {
        sm.inverse_bind[b] = glm::inverse(r.J[b]);
    }
    const int vt = outfit_design_tier(equip.outfit());

    switch (kind) {
        case OutfitKind::Plate: {
            // 0 squire: padded cloth gambeson; 1 knight: chainmail; 2 paladin: thick steel plate.
            const BodyMaterial body = vt == 0 ? BodyMaterial::Primary : BodyMaterial::Metal;
            const f32 bulk = vt == 2 ? 0.16f : vt == 1 ? 0.08f : 0.02f; // plate thickest, mail medium
            clad_torso(sm, r, 1.12f + bulk, 0.86f, body);
            clad_limb(sm, r, BonePart::UpperArmL, BonePart::LowerArmL, 1.16f + bulk, 1.0f, body, true);
            clad_limb(sm, r, BonePart::UpperArmR, BonePart::LowerArmR, 1.16f + bulk, 1.0f, body, true);
            clad_limb(sm, r, BonePart::UpperLegL, BonePart::LowerLegL, 1.12f + bulk, 0.95f, body, false);
            clad_limb(sm, r, BonePart::UpperLegR, BonePart::LowerLegR, 1.12f + bulk, 0.95f, body, false);
            if (vt == 0) {
                skirt(sm, r, 0.16f, 0.34f, BodyMaterial::Primary); // a short padded gambeson skirt
            }
            break;
        }
        case OutfitKind::Peasant: {
            // A plain belted tunic over the torso + cloth trousers; short sleeves leave the forearms bare.
            clad_torso(sm, r, 1.1f, 0.9f, BodyMaterial::Primary);
            clad_limb(sm, r, BonePart::UpperArmL, BonePart::LowerArmL, 1.08f, 0.45f, BodyMaterial::Primary, true);
            clad_limb(sm, r, BonePart::UpperArmR, BonePart::LowerArmR, 1.08f, 0.45f, BodyMaterial::Primary, true);
            clad_limb(sm, r, BonePart::UpperLegL, BonePart::LowerLegL, 1.08f, 0.92f, BodyMaterial::Pants, false);
            clad_limb(sm, r, BonePart::UpperLegR, BonePart::LowerLegR, 1.08f, 0.92f, BodyMaterial::Pants, false);
            skirt(sm, r, 0.12f, 0.3f, BodyMaterial::Primary); // a short tunic hem over the hips
            break;
        }
        case OutfitKind::Robe: {
            // Loose sleeves + a torso shell; the flowing skirt is a simulated ClothInstance now.
            clad_torso(sm, r, 1.18f, 0.84f, BodyMaterial::Primary);
            clad_limb(sm, r, BonePart::UpperArmL, BonePart::LowerArmL, vt == 0 ? 1.14f : 1.2f, 1.0f,
                      BodyMaterial::Primary, true);
            clad_limb(sm, r, BonePart::UpperArmR, BonePart::LowerArmR, vt == 0 ? 1.14f : 1.2f, 1.0f,
                      BodyMaterial::Primary, true);
            break;
        }
        case OutfitKind::Holy: {
            // Sleeves + a torso shell; the long flowing robe skirt is a simulated ClothInstance now.
            clad_torso(sm, r, 1.16f, 0.85f, BodyMaterial::Primary);
            clad_limb(sm, r, BonePart::UpperArmL, BonePart::LowerArmL, 1.16f, 1.0f, BodyMaterial::Primary, true);
            clad_limb(sm, r, BonePart::UpperArmR, BonePart::LowerArmR, 1.16f, 1.0f, BodyMaterial::Primary, true);
            break;
        }
        case OutfitKind::Leather: {
            // Olive tunic + trousers (hunter/warden); the beastmaster's is dark scale armour with sleeves.
            const BodyMaterial cloth = vt == 2 ? BodyMaterial::Dark : BodyMaterial::Primary;
            clad_torso(sm, r, vt == 2 ? 1.18f : 1.12f, 0.88f, cloth);
            clad_limb(sm, r, BonePart::UpperLegL, BonePart::LowerLegL, 1.12f, 1.0f, cloth, false);
            clad_limb(sm, r, BonePart::UpperLegR, BonePart::LowerLegR, 1.12f, 1.0f, cloth, false);
            if (vt == 2) { // scale sleeves over the arms
                clad_limb(sm, r, BonePart::UpperArmL, BonePart::LowerArmL, 1.14f, 0.8f, cloth, true);
                clad_limb(sm, r, BonePart::UpperArmR, BonePart::LowerArmR, 1.14f, 0.8f, cloth, true);
            }
            break;
        }
    }

    smooth_normals(sm);
    return sm;
}

} // namespace alryn
