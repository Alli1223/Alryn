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
    const int vt = outfit_design_tier(eq.outfit());
    const Vec3 ts = part_size(m, BonePart::Torso), tc = part_center(m, BonePart::Torso);
    const Vec3 hs = part_size(m, BonePart::Head), hc = part_center(m, BonePart::Head);

    // A leather belt + a hip pouch - common to the squire + knight tiers.
    auto leather_belt = [&]() {
        piece(m, BonePart::Pelvis, Vec3{0.0f, 0.03f, 0.0f},
              part_size(m, BonePart::Pelvis) * Vec3{1.2f, 0.42f, 1.22f}, BoneColor::Dark);
        piece(m, BonePart::Pelvis, Vec3{0.15f, -0.02f, ts.z * 0.42f}, Vec3{0.1f, 0.12f, 0.07f},
              BoneColor::Dark); // pouch
    };

    if (vt == 0) {
        // SQUIRE - a cloth coif hood framing the face, a shoulder drape, padded shoulder rolls, a belt.
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.06f, -hs.z * 0.2f},
              hs * Vec3{1.22f, 1.28f, 1.16f}, BoneColor::Dark); // hood shell pushed back so the face shows
        piece(m, BonePart::Head, Vec3{0.0f, hc.y - hs.y * 0.52f, 0.0f}, hs * Vec3{1.2f, 0.5f, 1.22f},
              BoneColor::Dark); // coif neck drape
        piece(m, BonePart::Torso, Vec3{0.0f, ts.y * 0.92f, 0.0f},
              Vec3{ts.x * 1.34f, ts.y * 0.3f, ts.z * 1.34f}, BoneColor::Dark); // coif cape on the shoulders
        for (BonePart up : {BonePart::UpperArmL, BonePart::UpperArmR}) {
            piece(m, up, Vec3{0.0f, -part_size(m, up).y * 0.04f, 0.0f}, Vec3{0.25f, 0.16f, 0.26f},
                  BoneColor::Primary, BoneShape::Sphere); // padded shoulder roll
        }
        leather_belt();
    } else if (vt == 1) {
        // KNIGHT - a conical nasal helm, crossed leather chest straps, steel pauldrons + knee cops.
        piece(m, BonePart::Head, hc, hs * Vec3{1.16f, 1.16f, 1.18f}, BoneColor::Metal); // helm dome
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.52f, 0.0f},
              Vec3{hs.x * 0.5f, hs.y * 0.5f, hs.z * 0.5f}, BoneColor::Metal, BoneShape::RoundedBox); // cone
        piece(m, BonePart::Head, Vec3{0.0f, hc.y - hs.y * 0.06f, hs.z * 0.62f},
              Vec3{0.05f, hs.y * 0.68f, 0.05f}, BoneColor::Metal, BoneShape::Box); // nasal bar
        for (f32 s : {1.0f, -1.0f}) {
            piece(m, BonePart::Torso, Vec3{0.0f, tc.y, ts.z * 0.6f}, Vec3{0.075f, ts.y * 1.34f, 0.04f},
                  BoneColor::Dark, BoneShape::Box, roll(s * 0.62f)); // X chest straps
        }
        for (BonePart up : {BonePart::UpperArmL, BonePart::UpperArmR}) {
            piece(m, up, Vec3{0.0f, -part_size(m, up).y * 0.02f, 0.0f}, Vec3{0.27f, 0.18f, 0.28f},
                  BoneColor::Metal, BoneShape::Sphere); // steel pauldron
        }
        for (BonePart up : {BonePart::UpperLegL, BonePart::UpperLegR}) {
            piece(m, up, Vec3{0.0f, -part_size(m, up).y, 0.02f}, Vec3{0.18f, 0.13f, 0.18f},
                  BoneColor::Metal, BoneShape::Sphere); // knee cop
        }
        leather_belt();
    } else {
        // PALADIN - angular faceted plate: a ridged breastplate, big layered pauldrons, tassets,
        // greaves + sabatons, an armet helm with a winged crest. Hard-edged Box plates (not soft
        // spheres) so each facet catches the light = a crisp low-poly knight. (Tabard + cape = cloth.)
        // Faceted breastplate proud of the chest, with a raised central ridge + gold-trim border.
        piece(m, BonePart::Torso, Vec3{0.0f, tc.y * 1.04f, ts.z * 0.52f},
              Vec3{ts.x * 1.16f, ts.y * 1.16f, ts.z * 0.5f}, BoneColor::Metal, BoneShape::Box); // cuirass
        piece(m, BonePart::Torso, Vec3{0.0f, tc.y * 1.18f, ts.z * 0.74f},
              Vec3{0.07f, ts.y * 0.92f, 0.1f}, BoneColor::Metal, BoneShape::Box, roll(0.0f)); // centre ridge
        piece(m, BonePart::Torso, Vec3{0.0f, ts.y * 0.5f, ts.z * 0.78f}, Vec3{ts.x * 1.18f, 0.05f, 0.05f},
              BoneColor::Accent, BoneShape::Box); // gold belt-line of the breastplate
        piece(m, BonePart::Torso, Vec3{0.0f, tc.y * 1.5f, ts.z * 0.6f},
              Vec3{ts.x * 0.44f, ts.x * 0.44f, 0.04f}, BoneColor::Accent, BoneShape::Box, roll(0.78f)); // sun crest (diamond)
        piece(m, BonePart::Torso, Vec3{0.0f, ts.y * 0.98f, 0.0f}, Vec3{ts.x * 1.22f, 0.08f, ts.z * 1.46f},
              BoneColor::Accent, BoneShape::Box); // gold gorget collar
        // Big angular pauldrons: a broad top plate tilted out + a smaller lame below (layered), gold rim.
        for (f32 ex : {1.0f, -1.0f}) {
            const BonePart up = ex > 0.0f ? BonePart::UpperArmL : BonePart::UpperArmR;
            const f32 uy = part_size(m, up).y;
            piece(m, up, Vec3{ex * 0.04f, -uy * 0.04f, 0.0f}, Vec3{0.34f, 0.2f, 0.36f}, BoneColor::Metal,
                  BoneShape::Box, roll(ex * 0.34f)); // top plate, tilted outward
            piece(m, up, Vec3{ex * 0.05f, -uy * 0.22f, 0.0f}, Vec3{0.32f, 0.13f, 0.34f}, BoneColor::Metal,
                  BoneShape::Box, roll(ex * 0.28f)); // lower lame
            piece(m, up, Vec3{ex * 0.03f, uy * 0.06f, 0.0f}, Vec3{0.36f, 0.05f, 0.38f}, BoneColor::Accent,
                  BoneShape::Box, roll(ex * 0.34f)); // gold rim along the top
        }
        // Angular gauntlets + greaves + sabatons (boxes, hard edges).
        for (BonePart lo : {BonePart::LowerArmL, BonePart::LowerArmR}) {
            piece(m, lo, Vec3{0.0f, -part_size(m, lo).y * 0.95f, 0.02f}, Vec3{0.17f, 0.17f, 0.18f},
                  BoneColor::Metal, BoneShape::Box); // gauntlet
        }
        for (BonePart lo : {BonePart::LowerLegL, BonePart::LowerLegR}) {
            piece(m, lo, Vec3{0.0f, -part_size(m, lo).y * 0.2f, 0.03f},
                  part_size(m, lo) * Vec3{1.3f, 0.86f, 1.32f}, BoneColor::Metal, BoneShape::Box); // greave
        }
        for (BonePart up : {BonePart::UpperLegL, BonePart::UpperLegR}) {
            piece(m, up, Vec3{0.0f, -part_size(m, up).y, 0.03f}, Vec3{0.21f, 0.14f, 0.22f}, BoneColor::Metal,
                  BoneShape::Box); // knee cop (faceted)
        }
        for (BonePart fp : {BonePart::FootL, BonePart::FootR}) {
            piece(m, fp, part_center(m, fp), part_size(m, fp) * Vec3{1.16f, 1.2f, 1.12f}, BoneColor::Metal,
                  BoneShape::Box); // sabaton
        }
        // Tassets: two angular plates hanging at the front of the hips.
        for (f32 ex : {-1.0f, 1.0f}) {
            piece(m, BonePart::Pelvis, Vec3{ex * 0.11f, -0.14f, part_size(m, BonePart::Pelvis).z * 0.55f},
                  Vec3{0.16f, 0.2f, 0.05f}, BoneColor::Metal, BoneShape::Box, roll(ex * 0.12f)); // tasset
            piece(m, BonePart::Pelvis, Vec3{ex * 0.11f, -0.04f, part_size(m, BonePart::Pelvis).z * 0.57f},
                  Vec3{0.17f, 0.04f, 0.05f}, BoneColor::Accent, BoneShape::Box); // gold tasset rim
        }
        leather_belt();
        // Armet helm: an angular dome + a brow guard + a lit visor slit + a winged gold crest.
        piece(m, BonePart::Head, Vec3{hc.x, hc.y + hs.y * 0.04f, hc.z}, hs * Vec3{1.2f, 1.3f, 1.22f},
              BoneColor::Metal, BoneShape::Box); // helm shell (angular)
        piece(m, BonePart::Head, Vec3{0.0f, hc.y - hs.y * 0.24f, hs.z * 0.5f},
              Vec3{hs.x * 1.08f, hs.y * 0.4f, hs.z * 0.7f}, BoneColor::Metal, BoneShape::Box,
              pitch(0.12f)); // angled chin visor
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.02f, hs.z * 0.74f},
              Vec3{hs.x * 0.96f, hs.y * 0.12f, 0.05f}, BoneColor::Glow, BoneShape::Box); // lit eye-slit
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.26f, hs.z * 0.46f},
              Vec3{hs.x * 1.24f, 0.06f, hs.z * 0.94f}, BoneColor::Accent, BoneShape::Box); // gold brow
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.4f, -hs.z * 0.1f},
              Vec3{0.05f, hs.y * 0.62f, hs.z * 1.04f}, BoneColor::Accent, BoneShape::Box); // gold comb crest
        for (f32 ex : {-1.0f, 1.0f}) { // gold wings flaring off the crest
            piece(m, BonePart::Head, Vec3{ex * hs.x * 0.56f, hc.y + hs.y * 0.5f, -hs.z * 0.06f},
                  Vec3{0.05f, 0.12f, 0.32f}, BoneColor::Accent, BoneShape::Box, roll(ex * 0.6f));
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Mage - APPRENTICE (patched hooded robe + rope belt) -> ELEMENTALIST (runed robe + circlet + shoulder
// cowl + spellbook) -> ARCHMAGE (ornate gold-trimmed robe + gem crown + gold pauldrons + glowing runes).
void build_robe(CharacterModel& m, const Equipment& eq) {
    const int vt = outfit_design_tier(eq.outfit());
    const Vec3 ts = part_size(m, BonePart::Torso), tc = part_center(m, BonePart::Torso);
    const Vec3 hs = part_size(m, BonePart::Head), hc = part_center(m, BonePart::Head);

    // A glowing/gold runic band down the robe front (the arcane orphrey).
    auto runes = [&](BoneColor c, f32 w) {
        piece(m, BonePart::Torso, Vec3{0.0f, tc.y * 0.7f, ts.z * 0.62f}, Vec3{w, ts.y * 1.42f, 0.03f}, c,
              BoneShape::Box);
    };
    // An angular high collar / shoulder cowl standing up around the neck.
    auto cowl = [&](f32 w) {
        piece(m, BonePart::Torso, Vec3{0.0f, ts.y * 0.86f, 0.0f}, Vec3{ts.x * w, ts.y * 0.5f, ts.z * 1.22f},
              BoneColor::Primary, BoneShape::Box);
    };

    if (vt == 0) {
        // APPRENTICE - a soft drawn-up hood with a drooping point (the face shows), a cowl drape, a
        // rope belt + pouch. Rounded, not a box head.
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.12f, -hs.z * 0.24f},
              hs * Vec3{1.22f, 1.3f, 1.16f}, BoneColor::Primary); // hood shell, pushed back so the face shows
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.52f, -hs.z * 0.34f},
              Vec3{hs.x * 0.46f, hs.y * 0.66f, hs.z * 0.5f}, BoneColor::Primary, BoneShape::RoundedBox,
              pitch(-0.42f)); // soft drooping peak
        piece(m, BonePart::Head, Vec3{0.0f, hc.y - hs.y * 0.5f, -hs.z * 0.06f},
              hs * Vec3{1.2f, 0.56f, 1.22f}, BoneColor::Primary); // cowl drape around the neck
        piece(m, BonePart::Pelvis, Vec3{0.0f, 0.04f, 0.0f},
              part_size(m, BonePart::Pelvis) * Vec3{1.16f, 0.3f, 1.18f}, BoneColor::Dark); // rope belt
        piece(m, BonePart::Pelvis, Vec3{0.15f, -0.02f, ts.z * 0.4f}, Vec3{0.09f, 0.11f, 0.06f},
              BoneColor::Dark); // pouch
    } else if (vt == 1) {
        // ELEMENTALIST - a circlet + gem, an angular shoulder cowl, a runed orphrey, a belt + spellbook.
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.42f, 0.0f}, hs * Vec3{1.18f, 0.18f, 1.18f},
              BoneColor::Accent, BoneShape::Box); // circlet
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.52f, hs.z * 0.5f}, Vec3{0.05f, 0.06f, 0.05f},
              BoneColor::Glow, BoneShape::Box); // circlet gem
        cowl(1.34f);
        runes(BoneColor::Accent, 0.055f);
        piece(m, BonePart::Pelvis, Vec3{0.0f, 0.04f, 0.0f},
              part_size(m, BonePart::Pelvis) * Vec3{1.16f, 0.34f, 1.18f}, BoneColor::Dark,
              BoneShape::Box); // belt
        piece(m, BonePart::Pelvis, Vec3{0.17f, 0.0f, ts.z * 0.32f}, Vec3{0.1f, 0.13f, 0.05f},
              BoneColor::Dark, BoneShape::Box); // spellbook at the hip
    } else {
        // ARCHMAGE - a pointed gem crown, an angular high collar, gold pauldrons + gems, gold + glowing runes.
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.46f, 0.0f}, hs * Vec3{1.16f, 0.24f, 1.16f},
              BoneColor::Accent, BoneShape::Box); // crown band
        for (int i = 0; i < 5; ++i) {
            const f32 a = (-0.5f + static_cast<f32>(i) * 0.25f) * Pi;
            piece(m, BonePart::Head,
                  Vec3{std::sin(a) * hs.x * 0.62f, hc.y + hs.y * 0.72f, std::cos(a) * hs.z * 0.62f},
                  Vec3{0.045f, 0.18f, 0.045f}, BoneColor::Accent, BoneShape::Box, roll(0.0f)); // tall points
        }
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.62f, hs.z * 0.6f}, Vec3{0.06f, 0.07f, 0.05f},
              BoneColor::Glow, BoneShape::Box); // front gem
        cowl(1.42f);
        piece(m, BonePart::Torso, Vec3{0.0f, ts.y * 1.0f, 0.0f}, Vec3{ts.x * 1.48f, 0.05f, ts.z * 1.46f},
              BoneColor::Accent, BoneShape::Box); // gold collar rim
        for (BonePart up : {BonePart::UpperArmL, BonePart::UpperArmR}) {
            piece(m, up, Vec3{0.0f, -part_size(m, up).y * 0.04f, 0.0f}, Vec3{0.24f, 0.14f, 0.26f},
                  BoneColor::Accent, BoneShape::Box); // gold pauldron (angular)
            piece(m, up, Vec3{0.0f, part_size(m, up).y * 0.04f, 0.08f}, Vec3{0.06f, 0.06f, 0.06f},
                  BoneColor::Glow, BoneShape::Box); // gem
        }
        runes(BoneColor::Accent, 0.07f); // gold rune band
        piece(m, BonePart::Torso, Vec3{0.0f, tc.y * 0.7f, ts.z * 0.64f}, Vec3{0.028f, ts.y * 1.3f, 0.03f},
              BoneColor::Glow, BoneShape::Box); // glowing rune line
        piece(m, BonePart::Pelvis, Vec3{0.0f, 0.04f, 0.0f},
              part_size(m, BonePart::Pelvis) * Vec3{1.18f, 0.4f, 1.2f}, BoneColor::Dark,
              BoneShape::Box); // belt
        piece(m, BonePart::Pelvis, Vec3{0.0f, 0.04f, part_size(m, BonePart::Pelvis).z * 0.62f},
              Vec3{0.07f, 0.06f, 0.04f}, BoneColor::Accent, BoneShape::Box); // gold buckle
    }
}

// ------------------------------------------------------------------------------------------------
// Hunter - HUNTER (leather jerkin + cap + mask + bandolier + quiver) -> WARDEN (a shoulder mantle +
// steel pauldron + extra straps + knee pads) -> BEASTMASTER (a bone skull mask + bone spikes + a
// tattered cape + glowing runes over dark scale).
void build_leather(CharacterModel& m, const Equipment& eq) {
    const int vt = outfit_design_tier(eq.outfit());
    const Vec3 ts = part_size(m, BonePart::Torso), tc = part_center(m, BonePart::Torso);
    const Vec3 hs = part_size(m, BonePart::Head), hc = part_center(m, BonePart::Head);

    // Shared ranger kit, all hard-edged Box leather (not soft blobs): a layered jerkin + shoulder yoke,
    // a buckled bandolier, a leather pauldron on the bow shoulder, a belt + pouches, vambraces, shin
    // wraps, boots, and a quiver of arrows angled across the back.
    piece(m, BonePart::Torso, Vec3{0.0f, tc.y * 0.9f, ts.z * 0.46f},
          Vec3{ts.x * 1.16f, ts.y * 1.18f, ts.z * 0.5f}, BoneColor::Dark, BoneShape::Box); // jerkin body (proud)
    piece(m, BonePart::Torso, Vec3{0.0f, ts.y * 0.74f, 0.0f}, Vec3{ts.x * 1.3f, ts.y * 0.46f, ts.z * 1.1f},
          BoneColor::Primary, BoneShape::Box); // shoulder yoke / collar (cloth colour)
    piece(m, BonePart::Torso, Vec3{0.0f, tc.y, ts.z * 0.66f}, Vec3{0.075f, ts.y * 1.36f, 0.05f},
          BoneColor::Dark, BoneShape::Box, roll(0.6f)); // bandolier
    piece(m, BonePart::Torso, Vec3{0.12f, tc.y * 1.18f, ts.z * 0.66f}, Vec3{0.06f, 0.06f, 0.04f},
          BoneColor::Accent, BoneShape::Box); // bandolier buckle
    {
        const f32 uy = part_size(m, BonePart::UpperArmL).y; // bow-arm (player's right) leather pauldron
        piece(m, BonePart::UpperArmL, Vec3{0.03f, -uy * 0.02f, 0.0f}, Vec3{0.3f, 0.18f, 0.32f},
              BoneColor::Dark, BoneShape::Box, roll(0.3f)); // pauldron top plate
        piece(m, BonePart::UpperArmL, Vec3{0.04f, -uy * 0.2f, 0.0f}, Vec3{0.28f, 0.12f, 0.3f},
              BoneColor::Dark, BoneShape::Box, roll(0.24f)); // lower lame
    }
    piece(m, BonePart::Pelvis, Vec3{0.0f, 0.03f, 0.0f},
          part_size(m, BonePart::Pelvis) * Vec3{1.2f, 0.42f, 1.22f}, BoneColor::Dark, BoneShape::Box); // belt
    piece(m, BonePart::Pelvis, Vec3{0.0f, 0.03f, part_size(m, BonePart::Pelvis).z * 0.6f},
          Vec3{0.08f, 0.07f, 0.04f}, BoneColor::Accent, BoneShape::Box); // belt buckle
    for (f32 ex : {-1.0f, 1.0f}) {
        piece(m, BonePart::Pelvis, Vec3{ex * 0.16f, -0.02f, ts.z * 0.3f}, Vec3{0.1f, 0.13f, 0.08f},
              BoneColor::Dark, BoneShape::Box); // hip pouches
    }
    for (BonePart lo : {BonePart::LowerArmL, BonePart::LowerArmR}) {
        piece(m, lo, Vec3{0.0f, -part_size(m, lo).y * 0.55f, 0.0f},
              Vec3{0.13f, part_size(m, lo).y * 0.74f, 0.14f}, BoneColor::Dark, BoneShape::Box); // vambrace
    }
    for (BonePart lo : {BonePart::LowerLegL, BonePart::LowerLegR}) {
        piece(m, lo, Vec3{0.0f, -part_size(m, lo).y * 0.4f, 0.04f},
              part_size(m, lo) * Vec3{1.22f, 0.52f, 1.26f}, BoneColor::Dark, BoneShape::Box); // shin wrap
    }
    for (BonePart fp : {BonePart::FootL, BonePart::FootR}) {
        piece(m, fp, part_center(m, fp), part_size(m, fp) * Vec3{1.12f, 1.16f, 1.1f}, BoneColor::Dark,
              BoneShape::Box); // boots
    }
    piece(m, BonePart::Torso, Vec3{0.15f, ts.y * 0.55f, -ts.z * 0.72f}, Vec3{0.12f, ts.y * 0.98f, 0.12f},
          BoneColor::Dark, BoneShape::Box, roll(-0.18f)); // quiver across the back
    for (int i = 0; i < 4; ++i) {
        const f32 ax = 0.11f + static_cast<f32>(i) * 0.04f;
        piece(m, BonePart::Torso, Vec3{ax, ts.y * 1.32f, -ts.z * 0.72f}, Vec3{0.012f, ts.y * 0.46f, 0.012f},
              BoneColor::Dark, BoneShape::Box); // arrow shaft
        piece(m, BonePart::Torso, Vec3{ax, ts.y * 1.54f, -ts.z * 0.72f}, Vec3{0.05f, 0.08f, 0.012f},
              vt == 0 ? BoneColor::Primary : BoneColor::Accent, BoneShape::Box); // fletching
    }

    if (vt == 0 || vt == 1) {
        // A pulled-up RANGER HOOD framing the face (soft shell + a peak swept back) + a face mask over
        // the lower face. Rounded + pushed back so the face shows, not a box head.
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.12f, -hs.z * 0.22f},
              hs * Vec3{1.24f, 1.3f, 1.18f}, BoneColor::Dark); // hood shell, pushed back so the face shows
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.46f, -hs.z * 0.5f},
              Vec3{hs.x * 0.5f, hs.y * 0.66f, hs.z * 0.62f}, BoneColor::Dark, BoneShape::RoundedBox,
              pitch(-0.5f)); // peak swept back
        piece(m, BonePart::Head, Vec3{0.0f, hc.y - hs.y * 0.18f, hs.z * 0.5f},
              Vec3{hs.x * 0.78f, hs.y * 0.42f, hs.z * 0.5f}, BoneColor::Primary,
              BoneShape::RoundedBox); // soft face mask over the lower face
    }
    if (vt == 1) {
        // WARDEN - a steel pauldron on the off shoulder + steel knee cops.
        piece(m, BonePart::UpperArmR, Vec3{-0.02f, -part_size(m, BonePart::UpperArmR).y * 0.02f, 0.0f},
              Vec3{0.28f, 0.18f, 0.3f}, BoneColor::Metal, BoneShape::Box, roll(-0.3f)); // steel pauldron
        for (BonePart up : {BonePart::UpperLegL, BonePart::UpperLegR}) {
            piece(m, up, Vec3{0.0f, -part_size(m, up).y, 0.03f}, Vec3{0.18f, 0.13f, 0.19f}, BoneColor::Metal,
                  BoneShape::Box); // steel knee cop
        }
    } else if (vt == 2) {
        // BEASTMASTER - an angular bone skull, sweeping horns, a fur ruff, bone pauldrons + spikes, runes.
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.04f, hs.z * 0.16f},
              hs * Vec3{1.16f, 1.16f, 1.16f}, BoneColor::Metal, BoneShape::Box); // skull (angular = bone)
        piece(m, BonePart::Head, Vec3{0.0f, hc.y - hs.y * 0.32f, hs.z * 0.6f},
              Vec3{hs.x * 0.52f, hs.y * 0.34f, hs.z * 0.46f}, BoneColor::Metal, BoneShape::Box); // snout
        for (f32 ex : {-1.0f, 1.0f}) {
            piece(m, BonePart::Head, Vec3{ex * hs.x * 0.34f, hc.y + hs.y * 0.06f, hs.z * 0.6f},
                  Vec3{0.05f, 0.06f, 0.04f}, BoneColor::Glow, BoneShape::Box); // glowing eyes
            piece(m, BonePart::Head, Vec3{ex * hs.x * 0.5f, hc.y + hs.y * 0.66f, -hs.z * 0.08f},
                  Vec3{0.05f, 0.32f, 0.05f}, BoneColor::Metal, BoneShape::Box, roll(ex * 0.42f)); // horn
        }
        piece(m, BonePart::Torso, Vec3{0.0f, ts.y * 0.82f, 0.0f},
              Vec3{ts.x * 1.42f, ts.y * 0.44f, ts.z * 1.42f}, BoneColor::Dark, BoneShape::Box); // fur ruff
        for (BonePart up : {BonePart::UpperArmL, BonePart::UpperArmR}) {
            piece(m, up, Vec3{0.0f, -part_size(m, up).y * 0.02f, 0.0f}, Vec3{0.28f, 0.2f, 0.3f},
                  BoneColor::Dark, BoneShape::Box); // bone pauldron
            piece(m, up, Vec3{0.0f, part_size(m, up).y * 0.2f, -0.05f}, Vec3{0.06f, 0.26f, 0.06f},
                  BoneColor::Metal, BoneShape::Box, pitch(-0.35f)); // bone spike angled back
        }
        piece(m, BonePart::Torso, Vec3{0.0f, tc.y, ts.z * 0.6f}, Vec3{0.035f, ts.y * 1.0f, 0.03f},
              BoneColor::Glow, BoneShape::Box); // glowing rune line
    }
}

// ------------------------------------------------------------------------------------------------
// Cleric - ACOLYTE (plain monk hood + corded belt + hung cross) -> PRIEST (circlet + white collar +
// a gold-cross stole + book) -> HIGH PROPHET (tall jewelled mitre + gold pauldrons + cross stole + cape).
void build_holy(CharacterModel& m, const Equipment& eq) {
    const int vt = outfit_design_tier(eq.outfit());
    const Vec3 ts = part_size(m, BonePart::Torso), tc = part_center(m, BonePart::Torso);
    const Vec3 hs = part_size(m, BonePart::Head), hc = part_center(m, BonePart::Head);

    // A small cross (vertical bar + arms) on the robe front at height y, size s.
    auto cross = [&](f32 y, f32 s) {
        piece(m, BonePart::Torso, Vec3{0.0f, y, ts.z * 0.64f}, Vec3{0.02f, s, 0.025f}, BoneColor::Accent,
              BoneShape::Box);
        piece(m, BonePart::Torso, Vec3{0.0f, y + s * 0.16f, ts.z * 0.64f}, Vec3{s * 0.5f, 0.02f, 0.025f},
              BoneColor::Accent, BoneShape::Box);
    };
    // A vertical gold orphrey band running down the front of the robe.
    auto orphrey = [&]() {
        piece(m, BonePart::Torso, Vec3{0.0f, tc.y * 0.6f, ts.z * 0.62f}, Vec3{0.075f, ts.y * 1.5f, 0.03f},
              BoneColor::Accent, BoneShape::Box);
    };
    // An angular amice collar over the shoulders (white cloth + a gold rim) - the priestly silhouette.
    auto amice = [&](f32 w) {
        piece(m, BonePart::Torso, Vec3{0.0f, ts.y * 0.84f, 0.0f}, Vec3{ts.x * w, ts.y * 0.46f, ts.z * 1.2f},
              BoneColor::Primary, BoneShape::Box);
        piece(m, BonePart::Torso, Vec3{0.0f, ts.y * 0.96f, 0.0f}, Vec3{ts.x * (w + 0.06f), 0.05f, ts.z * 1.3f},
              BoneColor::Accent, BoneShape::Box); // gold rim
    };

    if (vt == 0) {
        // ACOLYTE - a soft cowl hood framing the face (the face shows, like the squire coif), a cowl
        // drape over the neck/shoulders, a corded belt, a hung cross. Drab. Rounded, not a box head.
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.12f, -hs.z * 0.24f},
              hs * Vec3{1.22f, 1.32f, 1.16f}, BoneColor::Primary); // hood shell, pushed back so the face shows
        piece(m, BonePart::Head, Vec3{0.0f, hc.y - hs.y * 0.5f, -hs.z * 0.06f},
              hs * Vec3{1.2f, 0.56f, 1.22f}, BoneColor::Primary); // cowl drape around the neck
        piece(m, BonePart::Pelvis, Vec3{0.0f, 0.04f, 0.0f},
              part_size(m, BonePart::Pelvis) * Vec3{1.16f, 0.32f, 1.18f}, BoneColor::Dark); // cord belt
        cross(tc.y * 0.4f, ts.y * 0.42f);
    } else if (vt == 1) {
        // PRIEST - a circlet, an angular amice collar, a gold orphrey + cross, a belt + book.
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.42f, 0.0f}, hs * Vec3{1.18f, 0.18f, 1.18f},
              BoneColor::Accent, BoneShape::Box); // circlet
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.52f, hs.z * 0.5f}, Vec3{0.05f, 0.06f, 0.05f},
              BoneColor::Glow, BoneShape::Box); // circlet gem
        amice(1.34f);
        orphrey();
        cross(tc.y * 0.66f, ts.y * 0.42f);
        piece(m, BonePart::Pelvis, Vec3{0.0f, 0.04f, 0.0f},
              part_size(m, BonePart::Pelvis) * Vec3{1.16f, 0.34f, 1.18f}, BoneColor::Dark,
              BoneShape::Box); // belt
        piece(m, BonePart::Pelvis, Vec3{0.17f, 0.0f, ts.z * 0.32f}, Vec3{0.1f, 0.13f, 0.05f},
              BoneColor::Dark, BoneShape::Box); // book
    } else {
        // HIGH PROPHET - a peaked jewelled MITRE (two plates leaning to a point + a glowing cross), an
        // amice collar, angular gold pauldrons + gems, a gold orphrey band.
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.44f, 0.0f}, hs * Vec3{1.1f, 0.18f, 1.04f},
              BoneColor::Accent, BoneShape::Box); // gold base band
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.94f, hs.z * 0.2f},
              Vec3{hs.x * 0.92f, hs.y * 1.0f, 0.08f}, BoneColor::Accent, BoneShape::Box,
              pitch(0.4f)); // front plate, leans back
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.94f, -hs.z * 0.2f},
              Vec3{hs.x * 0.92f, hs.y * 1.0f, 0.08f}, BoneColor::Accent, BoneShape::Box,
              pitch(-0.4f)); // back plate, leans forward -> they meet at a peak
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.78f, hs.z * 0.34f},
              Vec3{0.028f, hs.y * 0.42f, 0.02f}, BoneColor::Glow, BoneShape::Box); // cross vertical
        piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.88f, hs.z * 0.36f},
              Vec3{hs.x * 0.42f, 0.028f, 0.02f}, BoneColor::Glow, BoneShape::Box); // cross arms
        amice(1.42f);
        for (BonePart up : {BonePart::UpperArmL, BonePart::UpperArmR}) {
            piece(m, up, Vec3{0.0f, -part_size(m, up).y * 0.04f, 0.0f}, Vec3{0.26f, 0.16f, 0.28f},
                  BoneColor::Accent, BoneShape::Box); // gold pauldron (angular)
            piece(m, up, Vec3{0.0f, part_size(m, up).y * 0.04f, 0.1f}, Vec3{0.06f, 0.06f, 0.06f},
                  BoneColor::Glow, BoneShape::Box); // gem
        }
        orphrey();
        cross(tc.y * 0.5f, ts.y * 0.4f);
        piece(m, BonePart::Pelvis, Vec3{0.0f, 0.04f, 0.0f},
              part_size(m, BonePart::Pelvis) * Vec3{1.18f, 0.36f, 1.2f}, BoneColor::Accent,
              BoneShape::Box); // gold belt
    }
}

// ------------------------------------------------------------------------------------------------
// Peasant - generic NPC townsfolk garb: a belted tunic with a front apron and a soft cloth cap. No
// tiers (the body tunic/trousers are the skinned OutfitMesh; these are the decorative bits).
void build_peasant(CharacterModel& m, const Equipment& eq) {
    (void)eq;
    const Vec3 ts = part_size(m, BonePart::Torso);
    const Vec3 hs = part_size(m, BonePart::Head), hc = part_center(m, BonePart::Head);
    // Rope belt.
    piece(m, BonePart::Pelvis, Vec3{0.0f, 0.04f, 0.0f},
          part_size(m, BonePart::Pelvis) * Vec3{1.16f, 0.3f, 1.18f}, BoneColor::Dark);
    // A simple apron panel down the front.
    piece(m, BonePart::Torso, Vec3{0.0f, -ts.y * 0.08f, ts.z * 0.66f},
          Vec3{ts.x * 0.74f, ts.y * 1.02f, 0.025f}, BoneColor::Accent, BoneShape::Box);
    // A soft cloth cap on the crown (leaves the face).
    piece(m, BonePart::Head, Vec3{0.0f, hc.y + hs.y * 0.34f, -hs.z * 0.04f}, hs * Vec3{1.12f, 0.52f, 1.12f},
          BoneColor::Dark, BoneShape::RoundedBox);
}

} // namespace

void apply_outfit(CharacterModel& model, OutfitKind kind, const Equipment& equip) {
    CharacterPalette& pal = model.palette();

    if (kind == OutfitKind::Peasant) {
        // Earthy homespun, with a little per-NPC variety via the (re-purposed) tint index.
        static const Vec3 tunics[4] = {{0.56f, 0.43f, 0.29f},  // tan
                                       {0.44f, 0.46f, 0.34f},  // olive
                                       {0.40f, 0.32f, 0.27f},  // brown
                                       {0.52f, 0.40f, 0.42f}}; // dusty rose
        pal.primary = tunics[equip.outfit_tint % 4];
        pal.pants = Vec3{0.33f, 0.27f, 0.20f};
        pal.dark = Vec3{0.25f, 0.18f, 0.12f};   // belt / cap
        pal.accent = Vec3{0.42f, 0.35f, 0.24f}; // apron
        pal.shirt = pal.primary;
        build_peasant(model, equip);
        return;
    }

    const EquipmentTier ot = equip.outfit();
    pal.primary = outfit_tint_of(equip.outfit_tint);
    // Basic-tier gear is rough undyed cloth/leather - desaturate the chosen colour toward a drab
    // homespun so the starting kit reads as a squire/apprentice/acolyte, the rich colour arriving with
    // the rare + legendary designs (matching the reference art).
    if (outfit_design_tier(ot) == 0) {
        pal.primary = glm::mix(pal.primary, Vec3{0.52f, 0.49f, 0.43f}, 0.55f);
    }
    pal.accent = tier_accent(ot);
    // Steel GREY (not near-white): a mid-tone so plates read as metal with crisp facet contrast against
    // the body + gold trim. Polish (tier sheen) lifts it toward bright steel at legendary, stays dull at low tiers.
    pal.metal = Vec3{0.46f, 0.50f, 0.58f} * tier_sheen(ot) + Vec3{0.14f};
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
        case OutfitKind::Peasant: break; // handled above
    }
}

} // namespace alryn
