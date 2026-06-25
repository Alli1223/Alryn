#include <Alryn/Character/Outfit.h>

#include <glm/gtc/color_space.hpp>

namespace alryn {

namespace {

// Append an outfit piece parented to a body part. center/size are in the PARENT JOINT's frame, so
// the piece moves with that limb when the rig is posed. `rot` orients the shape (a diagonal strap,
// an angled mitre peak). Defaults to a soft faceted rounded box.
void piece(CharacterModel& m, BonePart parent, const Vec3& center, const Vec3& size, BoneColor color,
           BoneShape shape = BoneShape::RoundedBox, const Quat& rot = QuatIdentity) {
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
    b.box_rotation = rot;
    b.color = color;
    b.shape = shape;
    b.attachment = true; // equipment rides on top of the skinned body
    m.add_bone(b);
}
// A rotation about the Z (roll) axis - handy for diagonal straps + angled crests.
Quat roll(f32 radians) { return glm::angleAxis(radians, Vec3{0.0f, 0.0f, 1.0f}); }
// A rotation about the X (pitch) axis - for forward/back-leaning pieces (mitre peaks, plumes).
Quat pitch(f32 radians) { return glm::angleAxis(radians, Vec3{1.0f, 0.0f, 0.0f}); }

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

    // The breastplate / armoured limbs are the continuous skinned OutfitMesh now; these are the
    // decorative silhouette pieces layered on top.
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

    // A big rounded pauldron flowing off each shoulder + a gauntlet at each wrist + a knee cop - the
    // rounded silhouette accents over the skinned steel limbs.
    for (BonePart up : {BonePart::UpperArmL, BonePart::UpperArmR}) {
        const f32 al = part_size(m, up).y;
        piece(m, up, Vec3{0.0f, -al * 0.02f, 0.0f}, Vec3{0.29f, 0.26f, 0.30f}, body,
              BoneShape::Sphere); // rounded pauldron
        if (t >= 2) {
            piece(m, up, Vec3{0.0f, -al * 0.02f, 0.0f}, Vec3{0.31f, 0.08f, 0.32f}, BoneColor::Accent,
                  BoneShape::Sphere); // gold pauldron rim
        }
    }
    for (BonePart lo : {BonePart::LowerArmL, BonePart::LowerArmR}) {
        const f32 al = part_size(m, lo).y;
        piece(m, lo, Vec3{0.0f, -al * 1.0f, 0.02f}, Vec3{0.15f, 0.13f, 0.16f}, body); // gauntlet
    }
    for (BonePart up : {BonePart::UpperLegL, BonePart::UpperLegR}) {
        const f32 ul = part_size(m, up).y;
        piece(m, up, Vec3{0.0f, -ul, 0.02f}, Vec3{0.2f, 0.16f, 0.2f}, body, BoneShape::Sphere); // knee cop
    }

    // Great-helm enclosing the head (covers the face), a lit eye-slit, and a plume in the chosen colour.
    piece(m, BonePart::Head, Vec3{hc.x, hc.y - hs.y * 0.04f, hc.z}, hs * Vec3{1.22f, 1.34f, 1.24f}, body);
    piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.04f, hs.z * 0.64f},
          Vec3{hs.x * 0.94f, hs.y * 0.14f, 0.05f}, BoneColor::Glow, BoneShape::Box); // lit eye-slit
    if (t >= 2) { // a gold brow band on the helm
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.22f, hs.z * 0.5f},
              Vec3{hs.x * 1.2f, 0.05f, hs.z * 0.9f}, BoneColor::Accent, BoneShape::Box);
    }
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

    // The robe body / sleeves / skirt are the continuous skinned OutfitMesh now; a gold trim strip
    // runs down the front opening over it.
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

// ------------------------------------------------------------------------------------------------
// Hunter - LEATHER ranger: an olive tunic + leather jerkin, a diagonal bandolier, belt pouches,
// bracers, a cap + face mask (only the eyes show), and a back quiver of arrows.
void build_leather(CharacterModel& m, const Equipment& eq) {
    const u8 t = static_cast<u8>(eq.outfit());
    const Vec3 ts = part_size(m, BonePart::Torso), tc = part_center(m, BonePart::Torso);
    const Vec3 hs = part_size(m, BonePart::Head), hc = part_center(m, BonePart::Head);

    // The olive tunic + trousers are the continuous skinned OutfitMesh now; a leather jerkin panel
    // sits over the chest.
    piece(m, BonePart::Torso, Vec3{0.0f, tc.y * 0.95f, ts.z * 0.5f}, Vec3{ts.x * 0.8f, ts.y * 0.8f, 0.06f},
          BoneColor::Dark);
    // Diagonal bandolier strap across the chest + a couple of pouches.
    piece(m, BonePart::Torso, Vec3{0.0f, tc.y, ts.z * 0.62f}, Vec3{0.07f, ts.y * 1.3f, 0.04f},
          BoneColor::Dark, BoneShape::Box, roll(0.6f));
    if (t >= 1) {
        piece(m, BonePart::Pelvis, Vec3{0.0f, 0.03f, 0.0f},
              part_size(m, BonePart::Pelvis) * Vec3{1.18f, 0.4f, 1.2f}, BoneColor::Dark); // belt
        for (f32 ex : {-1.0f, 1.0f}) {
            piece(m, BonePart::Pelvis, Vec3{ex * 0.16f, 0.0f, ts.z * 0.3f}, Vec3{0.09f, 0.11f, 0.07f},
                  BoneColor::Dark); // hip pouches
        }
    }
    if (t >= 2) {
        piece(m, BonePart::Pelvis, Vec3{0.0f, 0.03f, part_size(m, BonePart::Pelvis).z * 0.62f},
              Vec3{0.06f, 0.05f, 0.04f}, BoneColor::Accent, BoneShape::Box); // belt buckle
    }
    // Bracers on the forearms; boots over the skinned trousers.
    for (BonePart lo : {BonePart::LowerArmL, BonePart::LowerArmR}) {
        const f32 al = part_size(m, lo).y;
        piece(m, lo, Vec3{0.0f, -al * 0.55f, 0.0f}, Vec3{0.115f, al * 0.7f, 0.12f}, BoneColor::Dark);
    }
    for (BonePart fp : {BonePart::FootL, BonePart::FootR}) {
        piece(m, fp, part_center(m, fp), part_size(m, fp) * Vec3{1.08f, 1.1f, 1.04f}, BoneColor::Dark);
    }
    // Leather cap over the crown + a face mask covering the lower face (only the eyes show).
    piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.32f, 0.0f}, hs * Vec3{1.12f, 0.6f, 1.12f},
          BoneColor::Dark);
    piece(m, BonePart::Head, Vec3{0.0f, hc.y - hs.y * 0.22f, hs.z * 0.42f},
          Vec3{hs.x * 0.92f, hs.y * 0.5f, hs.z * 0.7f}, BoneColor::Primary); // mask
    // Back quiver + arrows jutting above the right shoulder.
    piece(m, BonePart::Torso, Vec3{0.16f, ts.y * 0.7f, -ts.z * 0.7f}, Vec3{0.1f, ts.y * 0.7f, 0.1f},
          BoneColor::Dark, BoneShape::Cylinder);
    for (int i = 0; i < 4; ++i) {
        const f32 ax = 0.10f + static_cast<f32>(i) * 0.045f;
        piece(m, BonePart::Torso, Vec3{ax, ts.y * 1.25f, -ts.z * 0.7f}, Vec3{0.012f, ts.y * 0.5f, 0.012f},
              BoneColor::Dark, BoneShape::Box); // shaft
        piece(m, BonePart::Torso, Vec3{ax, ts.y * 1.5f, -ts.z * 0.7f}, Vec3{0.05f, 0.07f, 0.012f},
              BoneColor::Primary, BoneShape::Box); // fletching
    }
}

// ------------------------------------------------------------------------------------------------
// Cleric - a white/blue/gold HOLY robe: gold-trimmed pauldrons + gorget, a blue tabard panel, a
// steel helm under a tall golden bishop's MITRE, a long robe skirt. (dark = royal blue here.)
void build_holy(CharacterModel& m, const Equipment& eq) {
    (void)eq; // the cleric look is the same across tiers (the materials brighten via the palette)
    const Vec3 ts = part_size(m, BonePart::Torso), tc = part_center(m, BonePart::Torso);
    const Vec3 hs = part_size(m, BonePart::Head), hc = part_center(m, BonePart::Head);

    // The white robe body / sleeves / skirt are the continuous skinned OutfitMesh now; a blue tabard
    // panel + gold trim run down the front over it.
    piece(m, BonePart::Torso, Vec3{0.0f, tc.y * 0.85f, ts.z * 0.62f}, Vec3{ts.x * 0.42f, ts.y * 1.15f, 0.03f},
          BoneColor::Dark, BoneShape::Box); // blue tabard
    piece(m, BonePart::Torso, Vec3{0.0f, tc.y * 0.85f, ts.z * 0.66f}, Vec3{ts.x * 0.1f, ts.y * 1.1f, 0.02f},
          BoneColor::Accent, BoneShape::Box); // gold tabard stripe
    // Gold-edged pauldrons + a gold gorget collar.
    for (BonePart up : {BonePart::UpperArmL, BonePart::UpperArmR}) {
        piece(m, up, Vec3{0.0f, -part_size(m, up).y * 0.12f, 0.0f}, Vec3{0.2f, 0.15f, 0.22f},
              BoneColor::Metal);
        piece(m, up, Vec3{0.0f, -part_size(m, up).y * 0.02f, 0.0f}, Vec3{0.22f, 0.05f, 0.24f},
              BoneColor::Accent);
    }
    piece(m, BonePart::Torso, Vec3{0.0f, ts.y * 0.96f, 0.0f}, Vec3{ts.x * 1.2f, 0.07f, ts.z * 1.5f},
          BoneColor::Accent);
    // Steel helm with a lit visor slit, under a modest two-peaked golden mitre.
    piece(m, BonePart::Head, hc, hs * Vec3{1.12f, 1.1f, 1.14f}, BoneColor::Metal);
    piece(m, BonePart::Head, Vec3{0.0f, hc.y, hs.z * 0.6f}, Vec3{hs.x * 0.82f, hs.y * 0.13f, 0.04f},
          BoneColor::Glow, BoneShape::Box);
    piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.74f, 0.0f},
          Vec3{hs.x * 0.78f, hs.y * 0.6f, hs.z * 0.42f}, BoneColor::Accent); // mitre body (smaller)
    piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 1.06f, hs.z * 0.13f},
          Vec3{hs.x * 0.66f, hs.y * 0.48f, 0.06f}, BoneColor::Accent, BoneShape::Box, pitch(0.16f)); // front peak
    piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 1.06f, -hs.z * 0.13f},
          Vec3{hs.x * 0.66f, hs.y * 0.48f, 0.06f}, BoneColor::Accent, BoneShape::Box, pitch(-0.16f)); // back peak
    piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.44f, 0.0f},
          Vec3{hs.x * 1.0f, hs.y * 0.11f, hs.z * 1.02f}, BoneColor::Dark); // blue brow band
    // Blue coif draping at the back/shoulders.
    piece(m, BonePart::Torso, Vec3{0.0f, ts.y * 0.86f, -ts.z * 0.4f},
          Vec3{ts.x * 1.2f, ts.y * 0.5f, ts.z * 0.7f}, BoneColor::Dark);
}

} // namespace

void apply_outfit(CharacterModel& model, OutfitKind kind, const Equipment& equip) {
    const EquipmentTier ot = equip.outfit();
    CharacterPalette& pal = model.palette();
    pal.primary = outfit_tint_of(equip.outfit_tint);
    pal.accent = tier_accent(ot);
    pal.metal = Vec3{0.74f, 0.78f, 0.86f} * tier_sheen(ot) + Vec3{0.12f}; // bright polished steel
    // `dark` is the secondary panel colour: royal blue for the Cleric's heraldry, leather brown else.
    pal.dark = (kind == OutfitKind::Holy) ? Vec3{0.20f, 0.26f, 0.58f} : Vec3{0.26f, 0.18f, 0.11f};
    pal.glow = (kind == OutfitKind::Plate) ? Vec3{0.45f, 0.7f, 1.0f} : Vec3{0.5f, 0.85f, 1.0f};
    // Recolour the base body's cloth to a neutral under-layer, so any gap the outfit doesn't cover
    // reads as a dark under-tunic rather than the random per-seed shirt/pants colour.
    pal.shirt = Vec3{0.20f, 0.18f, 0.16f};
    pal.pants = Vec3{0.17f, 0.15f, 0.14f};

    switch (kind) {
        case OutfitKind::Plate: build_plate(model, equip); break;
        case OutfitKind::Robe: build_robe(model, equip); break;
        case OutfitKind::Leather: build_leather(model, equip); break;
        case OutfitKind::Holy: build_holy(model, equip); break;
    }
}

} // namespace alryn
