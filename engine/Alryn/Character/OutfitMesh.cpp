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
    const u8 t = static_cast<u8>(equip.outfit());

    switch (kind) {
        case OutfitKind::Plate: {
            // Full steel limbs + chest (gambeson Primary at the ragged tier, plate Metal above).
            const BodyMaterial body = t >= 1 ? BodyMaterial::Metal : BodyMaterial::Primary;
            clad_torso(sm, r, 1.22f, 0.86f, body);
            clad_limb(sm, r, BonePart::UpperArmL, BonePart::LowerArmL, 1.28f, 1.0f, body, true);
            clad_limb(sm, r, BonePart::UpperArmR, BonePart::LowerArmR, 1.28f, 1.0f, body, true);
            clad_limb(sm, r, BonePart::UpperLegL, BonePart::LowerLegL, 1.22f, 0.95f, body, false);
            clad_limb(sm, r, BonePart::UpperLegR, BonePart::LowerLegR, 1.22f, 0.95f, body, false);
            break;
        }
        case OutfitKind::Robe: {
            // Loose violet sleeves + a robe body + a long draping skirt.
            clad_torso(sm, r, 1.18f, 0.84f, BodyMaterial::Primary);
            clad_limb(sm, r, BonePart::UpperArmL, BonePart::LowerArmL, 1.2f, 1.0f, BodyMaterial::Primary, true);
            clad_limb(sm, r, BonePart::UpperArmR, BonePart::LowerArmR, 1.2f, 1.0f, BodyMaterial::Primary, true);
            skirt(sm, r, 0.7f, 0.42f, BodyMaterial::Primary);
            break;
        }
        case OutfitKind::Holy: {
            // White robe body + sleeves + a long robe skirt to the ankles.
            clad_torso(sm, r, 1.16f, 0.85f, BodyMaterial::Primary);
            clad_limb(sm, r, BonePart::UpperArmL, BonePart::LowerArmL, 1.16f, 1.0f, BodyMaterial::Primary, true);
            clad_limb(sm, r, BonePart::UpperArmR, BonePart::LowerArmR, 1.16f, 1.0f, BodyMaterial::Primary, true);
            skirt(sm, r, 0.96f, 0.44f, BodyMaterial::Primary);
            break;
        }
        case OutfitKind::Leather: {
            // Olive tunic over the chest + matching trousers over the legs (boots stay the body feet).
            clad_torso(sm, r, 1.12f, 0.88f, BodyMaterial::Primary);
            clad_limb(sm, r, BonePart::UpperLegL, BonePart::LowerLegL, 1.12f, 1.0f, BodyMaterial::Primary, false);
            clad_limb(sm, r, BonePart::UpperLegR, BonePart::LowerLegR, 1.12f, 1.0f, BodyMaterial::Primary, false);
            break;
        }
    }

    smooth_normals(sm);
    return sm;
}

} // namespace alryn
