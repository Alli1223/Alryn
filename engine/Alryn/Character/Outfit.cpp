#include <Alryn/Character/Outfit.h>

#include <glm/gtc/color_space.hpp>

namespace alryn {

namespace {

// Append an outfit piece parented to a body part. center/size are in the PARENT JOINT's frame, so
// the piece moves with that limb when the rig is posed. Defaults to a soft faceted rounded box.
void piece(CharacterModel& m, BonePart parent, const Vec3& center, const Vec3& size, BoneColor color,
           BoneShape shape = BoneShape::RoundedBox) {
    const int p = m.bone_index(parent);
    if (p < 0) {
        return;
    }
    Bone b;
    b.part = BonePart::None;
    b.parent = p;
    b.joint_offset = Vec3{0.0f};
    b.box_center = center;
    b.box_size = size;
    b.color = color;
    b.shape = shape;
    m.add_bone(b);
}

Vec3 part_size(const CharacterModel& m, BonePart part) {
    const int i = m.bone_index(part);
    return i < 0 ? Vec3{0.1f} : m.bones()[static_cast<usize>(i)].box_size;
}
Vec3 part_center(const CharacterModel& m, BonePart part) {
    const int i = m.bone_index(part);
    return i < 0 ? Vec3{0.0f} : m.bones()[static_cast<usize>(i)].box_center;
}

// ------------------------------------------------------------------------------------------------
// Knight - steel PLATE with gold trim, a plumed great-helm, a heraldic tabard in the chosen colour.
// Tier scales the materials (cloth gambeson -> bright plate) and adds the plume + gold filigree.
void build_plate(CharacterModel& m, const Equipment& eq) {
    const u8 t = static_cast<u8>(eq.outfit());
    const Vec3 ts = part_size(m, BonePart::Torso), tc = part_center(m, BonePart::Torso);
    const Vec3 hs = part_size(m, BonePart::Head), hc = part_center(m, BonePart::Head);
    const BoneColor body = t >= 1 ? BoneColor::Metal : BoneColor::Primary; // plate vs gambeson

    // Breastplate + back (a chunky shell over the torso).
    piece(m, BonePart::Torso, Vec3{0.0f, tc.y, 0.01f}, ts * Vec3{1.16f, 1.04f, 1.34f}, body);
    // Gold collar/gorget + chest filigree at the higher tiers.
    if (t >= 2) {
        piece(m, BonePart::Torso, Vec3{0.0f, ts.y * 0.96f, 0.0f}, Vec3{ts.x * 1.18f, 0.07f, ts.z * 1.5f},
              BoneColor::Accent, BoneShape::RoundedBox);
        piece(m, BonePart::Torso, Vec3{0.0f, tc.y * 1.05f, ts.z * 0.66f},
              Vec3{ts.x * 0.5f, ts.y * 0.5f, 0.03f}, BoneColor::Accent, BoneShape::Box);
    }
    // Heraldic tabard in the player's colour hanging down the front.
    piece(m, BonePart::Torso, Vec3{0.0f, tc.y * 0.7f, ts.z * 0.7f},
          Vec3{ts.x * 0.46f, ts.y * 1.18f, 0.03f}, BoneColor::Primary, BoneShape::Box);
    // Fauld (skirt of plates) at the waist.
    piece(m, BonePart::Pelvis, Vec3{0.0f, -0.02f, 0.02f},
          part_size(m, BonePart::Pelvis) * Vec3{1.25f, 1.1f, 1.35f}, body);

    // Pauldrons (shoulder caps) + vambraces.
    for (BonePart up : {BonePart::UpperArmL, BonePart::UpperArmR}) {
        const f32 al = part_size(m, up).y;
        piece(m, up, Vec3{0.0f, -al * 0.12f, 0.0f}, Vec3{0.20f, 0.16f, 0.22f}, body);
    }
    for (BonePart lo : {BonePart::LowerArmL, BonePart::LowerArmR}) {
        const f32 al = part_size(m, lo).y;
        piece(m, lo, Vec3{0.0f, -al * 0.5f, 0.0f}, Vec3{0.14f, al * 1.02f, 0.15f}, body); // vambrace
        piece(m, lo, Vec3{0.0f, -al * 1.0f, 0.02f}, Vec3{0.12f, 0.10f, 0.13f}, BoneColor::Metal); // gauntlet
    }

    // Greaves over the shins + plated sabatons (the feet box is already the boot).
    for (BonePart lo : {BonePart::LowerLegL, BonePart::LowerLegR}) {
        const f32 ll = part_size(m, lo).y;
        piece(m, lo, Vec3{0.0f, -ll * 0.5f, 0.0f}, Vec3{0.155f, ll * 0.96f, 0.17f}, body);
    }
    for (BonePart up : {BonePart::UpperLegL, BonePart::UpperLegR}) {
        const f32 ul = part_size(m, up).y;
        piece(m, up, Vec3{0.0f, -ul * 0.5f, 0.0f}, Vec3{0.17f, ul * 0.9f, 0.18f}, body); // cuisse
    }

    // Great-helm over the head, with a dark visor slit, and a red-feel plume in the chosen colour.
    piece(m, BonePart::Head, Vec3{hc.x, hc.y, hc.z}, hs * Vec3{1.2f, 1.16f, 1.22f}, body);
    piece(m, BonePart::Head, Vec3{0.0f, hc.y - hs.y * 0.05f, hs.z * 0.62f},
          Vec3{hs.x * 0.92f, hs.y * 0.16f, 0.04f}, BoneColor::Glow, BoneShape::Box); // lit eye-slit
    if (t >= 2) { // plume crest
        for (int i = 0; i < 4; ++i) {
            const f32 fz = -hs.z * 0.1f - static_cast<f32>(i) * hs.z * 0.18f;
            piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * (0.7f + 0.04f * static_cast<f32>(i)), fz},
                  Vec3{0.05f, hs.y * (0.6f - 0.06f * static_cast<f32>(i)), 0.12f}, BoneColor::Primary,
                  BoneShape::RoundedBox);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Mage - a long hooded ROBE in the chosen colour with gold trim, a shoulder cape, a belt, gloves and
// a hood drawn up over a shadowed face (glowing eyes at the top tiers).
void build_robe(CharacterModel& m, const Equipment& eq) {
    const u8 t = static_cast<u8>(eq.outfit());
    const Vec3 ts = part_size(m, BonePart::Torso), tc = part_center(m, BonePart::Torso);
    const Vec3 hs = part_size(m, BonePart::Head), hc = part_center(m, BonePart::Head);

    // Robe body over the torso + a gold trim strip down the front opening.
    piece(m, BonePart::Torso, Vec3{0.0f, tc.y, 0.0f}, ts * Vec3{1.14f, 1.04f, 1.2f}, BoneColor::Primary);
    piece(m, BonePart::Torso, Vec3{0.0f, tc.y, ts.z * 0.62f}, Vec3{0.05f, ts.y * 1.04f, 0.04f},
          BoneColor::Accent, BoneShape::Box);
    // Shoulder cape (a wide cowl over the shoulders) + gold collar.
    piece(m, BonePart::Torso, Vec3{0.0f, ts.y * 0.9f, 0.0f}, Vec3{ts.x * 1.35f, ts.y * 0.34f, ts.z * 1.35f},
          BoneColor::Primary);
    if (t >= 2) {
        piece(m, BonePart::Torso, Vec3{0.0f, ts.y * 1.0f, 0.0f},
              Vec3{ts.x * 1.42f, 0.05f, ts.z * 1.42f}, BoneColor::Accent, BoneShape::RoundedBox);
    }
    // Belt + buckle.
    piece(m, BonePart::Pelvis, Vec3{0.0f, 0.04f, 0.0f},
          part_size(m, BonePart::Pelvis) * Vec3{1.18f, 0.45f, 1.22f}, BoneColor::Dark);
    if (t >= 1) {
        piece(m, BonePart::Pelvis, Vec3{0.0f, 0.04f, part_size(m, BonePart::Pelvis).z * 0.62f},
              Vec3{0.07f, 0.06f, 0.04f}, BoneColor::Accent, BoneShape::Box);
    }
    // Long robe skirt: a flared cover over the upper legs (the silhouette's defining piece).
    for (BonePart up : {BonePart::UpperLegL, BonePart::UpperLegR}) {
        const f32 ul = part_size(m, up).y;
        piece(m, up, Vec3{0.0f, -ul * 0.7f, 0.0f}, Vec3{0.21f, ul * 1.7f, 0.23f}, BoneColor::Primary);
    }
    // Sleeves over the arms.
    for (BonePart up : {BonePart::UpperArmL, BonePart::UpperArmR}) {
        const f32 al = part_size(m, up).y;
        piece(m, up, Vec3{0.0f, -al * 0.55f, 0.0f}, Vec3{0.14f, al * 1.1f, 0.15f}, BoneColor::Primary);
    }
    // Hood drawn up over the head + a shadowed (dark) face, with glowing eyes at higher tiers.
    piece(m, BonePart::Head, Vec3{hc.x, hc.y + hs.y * 0.06f, -hs.z * 0.06f}, hs * Vec3{1.22f, 1.2f, 1.34f},
          BoneColor::Primary);
    piece(m, BonePart::Head, Vec3{0.0f, hc.y - hs.y * 0.04f, hs.z * 0.5f},
          Vec3{hs.x * 0.7f, hs.y * 0.5f, 0.16f}, BoneColor::Dark, BoneShape::RoundedBox); // shadowed face
    if (t >= 1) {
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.92f, -hs.z * 0.1f},
              Vec3{0.05f, hs.y * 0.2f, 0.05f}, BoneColor::Accent, BoneShape::Box); // hood peak trim
    }
    for (f32 ex : {-1.0f, 1.0f}) {
        piece(m, BonePart::Head, Vec3{ex * hs.x * 0.22f, hc.y + hs.y * 0.06f, hs.z * 0.56f},
              Vec3{hs.x * 0.16f, hs.y * 0.1f, 0.05f}, BoneColor::Glow, BoneShape::Sphere); // glowing eyes
    }
}

} // namespace

void apply_outfit(CharacterModel& model, OutfitKind kind, const Equipment& equip) {
    const EquipmentTier ot = equip.outfit();
    CharacterPalette& pal = model.palette();
    pal.primary = outfit_tint_of(equip.outfit_tint);
    pal.accent = tier_accent(ot);
    pal.metal = Vec3{0.62f, 0.66f, 0.74f} * tier_sheen(ot) + Vec3{0.06f}; // brighter steel at higher tiers
    pal.dark = Vec3{0.22f, 0.17f, 0.12f};
    pal.glow = Vec3{0.45f, 0.85f, 1.0f};

    switch (kind) {
        case OutfitKind::Plate: build_plate(model, equip); break;
        case OutfitKind::Robe: build_robe(model, equip); break;
        case OutfitKind::Leather: // built in a later loop - fall back to a plain tunic recolour
        case OutfitKind::Holy:
            piece(model, BonePart::Torso, part_center(model, BonePart::Torso),
                  part_size(model, BonePart::Torso) * Vec3{1.08f, 1.02f, 1.12f}, BoneColor::Primary);
            break;
    }
}

} // namespace alryn
