#include <Alryn/Character/Outfit.h>

#include <Alryn/Character/BodyMesh.h>

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
// A rotation about the X (pitch) axis - for forward/back-leaning pieces (mitre peaks, scabbards).
Quat pitch(f32 radians) { return glm::angleAxis(radians, Vec3{1.0f, 0.0f, 0.0f}); }

// Where the skinned garments are, in the joint frames the attachment pieces hang from - read off the
// same torso profile the body + outfit meshes are lofted through, so a chest cross, a buckle or a
// quiver sits ON the garment it decorates whatever the character's build or race.
struct Fit {
    const CharacterModel& m;
    std::vector<TorsoRing> prof;
    f32 span = 0.65f;    // pelvis joint -> neck base
    f32 torso_y0 = 0.04f; // the torso joint's height above the pelvis joint
    Vec3 hs{0.22f}, hc{0.0f}, hr{0.11f}; // head box size, centre (head frame) + skinned half-extents
    f32 ar = 0.074f, lr = 0.1f;          // arm / leg radius

    explicit Fit(const CharacterModel& model) : m(model), prof(torso_profile(model)) {
        const int iT = m.bone_index(BonePart::Torso), iH = m.bone_index(BonePart::Head);
        const int iA = m.bone_index(BonePart::UpperArmL), iL = m.bone_index(BonePart::UpperLegL);
        if (iT < 0 || iH < 0 || iA < 0 || iL < 0) {
            return;
        }
        const std::vector<Bone>& b = m.bones();
        torso_y0 = b[static_cast<usize>(iT)].joint_offset.y;
        span = torso_y0 + b[static_cast<usize>(iH)].joint_offset.y;
        hs = b[static_cast<usize>(iH)].box_size;
        hc = b[static_cast<usize>(iH)].box_center;
        hr = Vec3{hs.x * 0.5f, hs.y * 0.5f, hs.z * 0.52f};
        ar = b[static_cast<usize>(iA)].box_size.x * 0.5f;
        lr = b[static_cast<usize>(iL)].box_size.x * 0.5f;
    }
    TorsoRing at(f32 t) const { return torso_ring_at(prof, t); }
    f32 ty(f32 t) const { return t * span - torso_y0; } // torso-frame height of profile t
    f32 py(f32 t) const { return t * span; }            // pelvis-frame height of profile t
    // Torso-frame point on the FRONT / BACK of a garment `inflate` off the body at height t.
    Vec3 front(f32 t, f32 inflate) const {
        const TorsoRing r = at(t);
        return Vec3{0.0f, ty(t), r.dz + r.rz + inflate};
    }
    Vec3 back(f32 t, f32 inflate) const {
        const TorsoRing r = at(t);
        return Vec3{0.0f, ty(t), r.dz - r.rz - inflate};
    }
    f32 seg(BonePart p) const {
        const int i = m.bone_index(p);
        return i < 0 ? 0.3f : -2.0f * m.bones()[static_cast<usize>(i)].box_center.y;
    }
};

// The arm on the player's right (the rig's labels are mirrored) is the L-suffixed one at -X; `out`
// is the outward direction (+-1 on X) of either upper arm.
f32 outward(const CharacterModel& m, BonePart up) {
    const int i = m.bone_index(up);
    return (i >= 0 && m.bones()[static_cast<usize>(i)].joint_offset.x < 0.0f) ? -1.0f : 1.0f;
}

// Hair that would poke through headwear: everything on the crown (crown cap, fringe, spikes, crest,
// tail) for a hood / helm; for a hat, what stands up above the brim and the fringe it pulls down over
// (the hair at the sides + back still shows under the brim). A beard stays.
void hide_hair(CharacterModel& m, const Fit& f, bool whole_crown) {
    m.remove_attachments([&](const Bone& b) {
        if (b.color != BoneColor::Hair) {
            return false;
        }
        if (whole_crown) {
            return b.box_center.y > f.hc.y;
        }
        const bool tall = b.box_center.y + b.box_size.y * 0.5f > f.hc.y + f.hr.y * 1.3f;
        const bool fringe = b.box_center.y > f.hc.y && b.box_center.z > f.hc.z + f.hr.z * 0.5f;
        return tall || fringe;
    });
}

// A belt buckle (+ an optional hanging pouch) at profile height t on a belt `inflate` off the body.
void buckle(CharacterModel& m, const Fit& f, f32 t, f32 inflate, BoneColor color, bool pouch) {
    const Vec3 fr = f.front(t, inflate + 0.012f);
    piece(m, BonePart::Torso, fr, Vec3{0.06f, 0.055f, 0.024f}, color, BoneShape::Box);
    if (pouch) {
        const TorsoRing r = f.at(t);
        piece(m, BonePart::Pelvis, Vec3{r.rx * 0.82f + inflate, f.py(t) - 0.07f, r.dz + r.rz * 0.5f},
              Vec3{0.05f, 0.1f, 0.085f}, BoneColor::Dark); // a purse on the belt
    }
}

// A sheathed sword hanging at the player's LEFT hip (+X), angled back.
void scabbard(CharacterModel& m, const Fit& f, BoneColor sheath, BoneColor chape) {
    const TorsoRing r = f.at(-0.07f);
    const f32 x = std::max(r.rx, 0.1f + f.lr * 1.15f) + 0.05f;
    const f32 len = f.span * 0.95f;
    piece(m, BonePart::Pelvis, Vec3{x, -len * 0.36f, -0.05f}, Vec3{0.045f, len, 0.03f}, sheath, BoneShape::Box,
          pitch(-0.38f) * roll(0.06f));
    piece(m, BonePart::Pelvis, Vec3{x + 0.004f, -len * 0.82f, -0.2f}, Vec3{0.05f, 0.07f, 0.036f}, chape,
          BoneShape::Box, pitch(-0.38f)); // the metal chape at its tip
    piece(m, BonePart::Pelvis, Vec3{x - 0.005f, len * 0.13f, 0.04f}, Vec3{0.12f, 0.025f, 0.03f}, chape,
          BoneShape::Box, pitch(-0.38f)); // the hilt's crossguard showing at the mouth
}

// A quiver of arrows slung across the back, its mouth over the `side` shoulder.
void quiver(CharacterModel& m, const Fit& f, f32 side, BoneColor fletch) {
    const Vec3 bk = f.back(0.6f, 0.05f);
    const Quat tilt = roll(-side * 0.32f);
    const f32 len = f.span * 0.78f;
    piece(m, BonePart::Torso, Vec3{side * 0.06f, bk.y, bk.z - 0.04f}, Vec3{0.11f, len, 0.11f}, BoneColor::Dark,
          BoneShape::Box, tilt);
    for (int i = 0; i < 4; ++i) {
        const f32 ox = (static_cast<f32>(i) - 1.5f) * 0.026f;
        const Vec3 tip = Vec3{side * 0.06f, bk.y, bk.z - 0.04f} + tilt * Vec3{ox, len * 0.5f + 0.09f, 0.0f};
        piece(m, BonePart::Torso, tip, Vec3{0.012f, 0.2f, 0.012f}, BoneColor::Dark, BoneShape::Box, tilt);
        piece(m, BonePart::Torso, tip + tilt * Vec3{0.0f, 0.1f, 0.0f}, Vec3{0.04f, 0.07f, 0.012f}, fletch,
              BoneShape::Box, tilt); // fletching
    }
    // The strap across the chest.
    const Vec3 fr = f.front(0.62f, 0.05f);
    piece(m, BonePart::Torso, Vec3{0.0f, fr.y, fr.z}, Vec3{0.05f, f.span * 0.95f, 0.02f}, BoneColor::Dark,
          BoneShape::Box, roll(side * 0.62f));
}

// Layered plate pauldrons on both shoulders: a broad top plate tilted off the shoulder, a lame below
// and a trim rim along the top.
void pauldrons(CharacterModel& m, const Fit& f, BoneColor plate, BoneColor rim, f32 scale) {
    for (BonePart up : {BonePart::UpperArmL, BonePart::UpperArmR}) {
        const f32 o = outward(m, up);
        const f32 a = f.ar * scale;
        piece(m, up, Vec3{o * a * 0.35f, a * 0.3f, 0.0f}, Vec3{a * 3.6f, a * 2.0f, a * 3.7f}, plate,
              BoneShape::Box, roll(-o * 0.34f));
        piece(m, up, Vec3{o * a * 0.55f, -a * 1.15f, 0.0f}, Vec3{a * 3.1f, a * 1.2f, a * 3.4f}, plate,
              BoneShape::Box, roll(-o * 0.28f));
        piece(m, up, Vec3{o * a * 0.25f, a * 1.25f, 0.0f}, Vec3{a * 3.7f, 0.028f, a * 3.8f}, rim, BoneShape::Box,
              roll(-o * 0.34f));
    }
}

// ------------------------------------------------------------------------------------------------
// Knight - SQUIRE (quilted gambeson, wool hood) -> KNIGHT (mail hauberk + coif, a heraldic surcoat with
// a cross, a conical nasal helm, a cloak) -> PALADIN (full plate, layered pauldrons, a great helm with a
// gilt cross, a cape). The garments are the skinned OutfitMesh; these are the hard pieces on top.
void build_plate(CharacterModel& m, const Equipment& eq) {
    const Fit f(m);
    const int vt = outfit_design_tier(eq.outfit());
    const bool helm = !eq.bare_head; // the creator's turntable shows the face (no headwear)
    if (helm) {
        hide_hair(m, f, true); // a hood, a coif, a great helm
    }
    scabbard(m, f, BoneColor::Dark, vt == 0 ? BoneColor::Metal : BoneColor::Accent);

    if (vt == 0) {
        buckle(m, f, 0.22f, 0.05f, BoneColor::Metal, true);
    } else if (vt == 1) {
        // The surcoat's heraldic cross on the chest (its lower arm runs on down the cloth skirt).
        const Vec3 c = f.front(0.62f, 0.056f);
        piece(m, BonePart::Torso, Vec3{0.0f, c.y - 0.02f, c.z}, Vec3{0.075f, f.span * 0.52f, 0.02f},
              BoneColor::Accent, BoneShape::Box);
        piece(m, BonePart::Torso, Vec3{0.0f, f.ty(0.7f), f.front(0.7f, 0.058f).z},
              Vec3{f.at(0.7f).rx * 1.5f, 0.075f, 0.02f}, BoneColor::Accent, BoneShape::Box);
        buckle(m, f, 0.26f, 0.062f, BoneColor::Accent, false);
        // The helm's nasal bar down over the nose.
        if (helm) {
            piece(m, BonePart::Head, Vec3{0.0f, f.hc.y + f.hr.y * 0.05f, f.hr.z * 1.3f},
                  Vec3{0.03f, f.hr.y * 0.95f, 0.026f}, BoneColor::Metal, BoneShape::Box);
        }
        // Steel poleyns over the mail at the knees.
        for (BonePart up : {BonePart::UpperLegL, BonePart::UpperLegR}) {
            piece(m, up, Vec3{0.0f, -f.seg(up), f.lr * 1.0f}, Vec3{f.lr * 1.7f, f.lr * 1.4f, f.lr * 1.0f},
                  BoneColor::Metal, BoneShape::Sphere);
        }
        // A cloak brooch at the throat.
        piece(m, BonePart::Torso, f.front(0.9f, 0.06f), Vec3{0.05f}, BoneColor::Accent, BoneShape::Sphere);
    } else {
        // Gorget + layered pauldrons + gauntlets, a gilt sun-cross on the breastplate, a sword belt.
        const TorsoRing nk = f.at(0.95f);
        piece(m, BonePart::Torso, Vec3{0.0f, f.ty(0.95f), nk.dz}, Vec3{nk.rx * 2.0f + 0.1f, 0.06f, nk.rz * 2.0f + 0.1f},
              BoneColor::Metal, BoneShape::Cylinder); // gorget
        piece(m, BonePart::Torso, Vec3{0.0f, f.ty(0.95f) + 0.032f, nk.dz},
              Vec3{nk.rx * 2.0f + 0.11f, 0.014f, nk.rz * 2.0f + 0.11f}, BoneColor::Accent, BoneShape::Cylinder);
        pauldrons(m, f, BoneColor::Metal, BoneColor::Accent, 1.25f);
        const Vec3 c = f.front(0.64f, 0.072f);
        piece(m, BonePart::Torso, c, Vec3{0.05f, 0.2f, 0.02f}, BoneColor::Accent, BoneShape::Box);
        piece(m, BonePart::Torso, c + Vec3{0.0f, 0.03f, 0.0f}, Vec3{0.15f, 0.045f, 0.02f}, BoneColor::Accent,
              BoneShape::Box);
        buckle(m, f, 0.2f, 0.058f, BoneColor::Accent, false);
        for (BonePart lo : {BonePart::LowerArmL, BonePart::LowerArmR}) {
            piece(m, lo, Vec3{0.0f, -f.seg(lo) * 0.98f, 0.0f}, Vec3{f.ar * 2.2f, f.ar * 1.6f, f.ar * 2.3f},
                  BoneColor::Metal, BoneShape::Box); // gauntlet cuff
        }
        // The great helm's face: a dark eye slit, a gilt cross down the faceplate, a crest on top.
        if (!helm) {
            return;
        }
        const f32 fz = f.hr.z * 1.25f;
        piece(m, BonePart::Head, Vec3{0.0f, f.hc.y + f.hr.y * 0.12f, fz}, Vec3{f.hr.x * 1.9f, 0.026f, 0.03f},
              BoneColor::Dark, BoneShape::Box);
        piece(m, BonePart::Head, Vec3{0.0f, f.hc.y - f.hr.y * 0.32f, fz + 0.004f},
              Vec3{0.03f, f.hr.y * 0.75f, 0.022f}, BoneColor::Accent, BoneShape::Box);
        piece(m, BonePart::Head, Vec3{0.0f, f.hc.y + f.hr.y * 0.32f, fz - 0.004f},
              Vec3{f.hr.x * 1.7f, 0.028f, 0.022f}, BoneColor::Accent, BoneShape::Box); // brow band
        for (f32 bx : {-1.0f, 1.0f}) {
            for (int k = 0; k < 3; ++k) { // breaths below the slit on the cheek plates
                piece(m, BonePart::Head,
                      Vec3{bx * f.hr.x * 0.55f, f.hc.y - f.hr.y * (0.2f + 0.16f * static_cast<f32>(k)), fz - 0.012f},
                      Vec3{0.03f, 0.012f, 0.02f}, BoneColor::Dark, BoneShape::Box);
            }
        }
        piece(m, BonePart::Head, Vec3{0.0f, f.hc.y + f.hr.y * 1.12f, -0.01f}, Vec3{0.03f, f.hr.y * 0.42f, f.hr.z * 1.8f},
              BoneColor::Accent, BoneShape::Box); // crest comb
    }
}

// ------------------------------------------------------------------------------------------------
// Mage - APPRENTICE (hooded robe, rope belt, purse) -> ELEMENTALIST (pointed hat, mantle, spellbook) ->
// ARCHMAGE (tall hat with a star gem, a gilt high collar over the mantle, a gilt buckle).
void build_robe(CharacterModel& m, const Equipment& eq) {
    const Fit f(m);
    const int vt = outfit_design_tier(eq.outfit());
    if (!eq.bare_head) {
        hide_hair(m, f, vt == 0); // the hood covers it all; a hat only the tall hair
    }
    if (vt == 0) {
        buckle(m, f, 0.26f, 0.044f, BoneColor::Dark, true);
    } else {
        buckle(m, f, 0.26f, 0.05f, BoneColor::Accent, false);
        const TorsoRing r = f.at(0.26f);
        piece(m, BonePart::Pelvis, Vec3{-(r.rx + 0.06f), f.py(0.26f) - 0.1f, r.dz + 0.03f},
              Vec3{0.05f, 0.15f, 0.12f}, BoneColor::Dark, BoneShape::Box, roll(0.1f)); // spellbook at the hip
        piece(m, BonePart::Pelvis, Vec3{-(r.rx + 0.066f), f.py(0.26f) - 0.1f, r.dz + 0.03f},
              Vec3{0.04f, 0.12f, 0.1f}, BoneColor::Accent,
              BoneShape::Box, roll(0.1f)); // its gilt clasp edge
    }
    if (vt == 2 && !eq.bare_head) {
        // A glowing star gem on the hat band.
        piece(m, BonePart::Head, Vec3{0.0f, f.hc.y + f.hr.y * 0.5f + 0.06f, f.hr.z * 1.12f},
              Vec3{0.05f, 0.05f, 0.03f}, BoneColor::Glow, BoneShape::Box, roll(0.785f));
    }
}

// ------------------------------------------------------------------------------------------------
// Hunter - HUNTER (hooded tunic under a leather jerkin, a quiver + bandolier, purses) -> WARDEN (+ a
// steel pauldron + knee cops, a shoulder mantle) -> BEASTMASTER (a bone skull mask, horns, a fur ruff,
// bone pauldrons over dark scale, a cape).
void build_leather(CharacterModel& m, const Equipment& eq) {
    const Fit f(m);
    const int vt = outfit_design_tier(eq.outfit());
    if (!eq.bare_head) {
        hide_hair(m, f, true);
    }
    quiver(m, f, 1.0f, vt == 0 ? BoneColor::Primary : BoneColor::Accent);
    buckle(m, f, 0.2f, 0.056f, BoneColor::Accent, true);
    {
        // A leather pauldron on the bow shoulder (the player's right = the L-suffixed arm).
        const f32 o = outward(m, BonePart::UpperArmL);
        piece(m, BonePart::UpperArmL, Vec3{o * f.ar * 0.4f, f.ar * 0.2f, 0.0f},
              Vec3{f.ar * 3.4f, f.ar * 1.9f, f.ar * 3.6f}, BoneColor::Dark, BoneShape::Box, roll(-o * 0.3f));
        piece(m, BonePart::UpperArmL, Vec3{o * f.ar * 0.55f, -f.ar * 1.0f, 0.0f},
              Vec3{f.ar * 3.0f, f.ar * 1.2f, f.ar * 3.2f}, BoneColor::Dark, BoneShape::Box, roll(-o * 0.24f));
    }
    if (vt == 1) {
        // WARDEN - a steel pauldron on the off shoulder + steel knee cops.
        const f32 o = outward(m, BonePart::UpperArmR);
        piece(m, BonePart::UpperArmR, Vec3{o * f.ar * 0.4f, f.ar * 0.2f, 0.0f},
              Vec3{f.ar * 3.5f, f.ar * 2.0f, f.ar * 3.7f}, BoneColor::Metal, BoneShape::Box, roll(-o * 0.3f));
        for (BonePart up : {BonePart::UpperLegL, BonePart::UpperLegR}) {
            piece(m, up, Vec3{0.0f, -f.seg(up), f.lr * 0.95f}, Vec3{f.lr * 1.6f, f.lr * 1.3f, f.lr * 1.0f},
                  BoneColor::Metal, BoneShape::Box);
        }
    } else if (vt == 2) {
        // BEASTMASTER - an angular bone skull, sweeping horns, a fur ruff, bone pauldrons + spikes, runes.
        if (!eq.bare_head) {
            piece(m, BonePart::Head, Vec3{0.0f, f.hc.y + f.hs.y * 0.04f, f.hs.z * 0.14f}, f.hs * Vec3{1.18f, 1.16f, 1.16f},
                  BoneColor::Metal, BoneShape::Box); // skull (angular = bone)
            piece(m, BonePart::Head, Vec3{0.0f, f.hc.y - f.hs.y * 0.3f, f.hs.z * 0.62f},
                  Vec3{f.hs.x * 0.52f, f.hs.y * 0.34f, f.hs.z * 0.46f}, BoneColor::Metal, BoneShape::Box); // snout
            for (f32 ex : {-1.0f, 1.0f}) {
                piece(m, BonePart::Head, Vec3{ex * f.hs.x * 0.24f, f.hc.y + f.hs.y * 0.06f, f.hs.z * 0.6f},
                      Vec3{0.045f, 0.05f, 0.04f}, BoneColor::Glow, BoneShape::Box); // glowing eyes
                piece(m, BonePart::Head, Vec3{ex * f.hs.x * 0.5f, f.hc.y + f.hs.y * 0.66f, -f.hs.z * 0.08f},
                      Vec3{0.045f, f.hs.y * 1.0f, 0.045f}, BoneColor::Metal, BoneShape::Box, roll(ex * 0.42f)); // horn
            }
        }
        const TorsoRing sh = f.at(0.86f);
        piece(m, BonePart::Torso, Vec3{0.0f, f.ty(0.86f), sh.dz}, Vec3{sh.rx * 2.6f, f.span * 0.24f, sh.rz * 2.9f},
              BoneColor::Dark, BoneShape::RoundedBox); // fur ruff
        for (BonePart up : {BonePart::UpperArmL, BonePart::UpperArmR}) {
            const f32 o = outward(m, up);
            piece(m, up, Vec3{o * f.ar * 0.4f, f.ar * 0.2f, 0.0f}, Vec3{f.ar * 3.6f, f.ar * 2.4f, f.ar * 3.8f},
                  BoneColor::Dark, BoneShape::Box, roll(-o * 0.3f)); // bone pauldron
            piece(m, up, Vec3{o * f.ar * 0.5f, f.ar * 2.2f, -0.04f}, Vec3{0.05f, 0.22f, 0.05f}, BoneColor::Metal,
                  BoneShape::Box, pitch(-0.35f) * roll(-o * 0.4f)); // bone spike angled back
        }
        piece(m, BonePart::Torso, f.front(0.55f, 0.045f), Vec3{0.03f, f.span * 0.5f, 0.02f}, BoneColor::Glow,
              BoneShape::Box); // glowing rune line
    }
}

// ------------------------------------------------------------------------------------------------
// Cleric - ACOLYTE (monk's cowl, rope girdle, a wooden cross) -> PRIEST (circlet, a gilt orphrey + cross,
// a book) -> HIGH PROPHET (a jewelled mitre, a gilt orphrey + cross, a cope).
void build_holy(CharacterModel& m, const Equipment& eq) {
    const Fit f(m);
    const int vt = outfit_design_tier(eq.outfit());
    if (!eq.bare_head) {
        hide_hair(m, f, vt != 1);
    }
    // A cross on the chest at height t, arm-length s, standing `inflate` proud of the body.
    auto cross = [&](f32 t, f32 s, f32 inflate, BoneColor c) {
        const Vec3 p = f.front(t, inflate);
        piece(m, BonePart::Torso, p, Vec3{0.026f, s, 0.02f}, c, BoneShape::Box);
        piece(m, BonePart::Torso, p + Vec3{0.0f, s * 0.18f, 0.002f}, Vec3{s * 0.6f, 0.026f, 0.02f}, c, BoneShape::Box);
    };
    // A gilt orphrey band down the front of the vestment.
    auto orphrey = [&]() {
        const Vec3 p = f.front(0.55f, 0.034f);
        piece(m, BonePart::Torso, p, Vec3{0.07f, f.span * 0.62f, 0.016f}, BoneColor::Accent, BoneShape::Box);
    };
    if (vt == 0) {
        cross(0.5f, 0.12f, 0.05f, BoneColor::Dark); // a plain wooden cross on a cord
        const TorsoRing r = f.at(0.26f);
        piece(m, BonePart::Pelvis, Vec3{r.rx * 0.5f, f.py(0.26f) - 0.14f, r.dz + r.rz + 0.05f},
              Vec3{0.025f, 0.26f, 0.025f}, BoneColor::Accent, BoneShape::Box); // the girdle's hanging cord
    } else if (vt == 1) {
        piece(m, BonePart::Head, Vec3{0.0f, f.hc.y + f.hs.y * 0.4f, 0.0f}, f.hs * Vec3{1.12f, 0.14f, 1.12f},
              BoneColor::Accent, BoneShape::Cylinder); // circlet
        piece(m, BonePart::Head, Vec3{0.0f, f.hc.y + f.hs.y * 0.42f, f.hs.z * 0.56f}, Vec3{0.04f},
              BoneColor::Glow, BoneShape::Box); // circlet gem
        orphrey();
        cross(0.66f, 0.13f, 0.05f, BoneColor::Accent);
        buckle(m, f, 0.26f, 0.044f, BoneColor::Accent, false);
        const TorsoRing r = f.at(0.26f);
        piece(m, BonePart::Pelvis, Vec3{-(r.rx + 0.05f), f.py(0.26f) - 0.1f, r.dz + 0.02f},
              Vec3{0.05f, 0.14f, 0.11f}, BoneColor::Dark, BoneShape::Box); // book at the hip
    } else {
        // A peaked jewelled MITRE (two plates leaning to a point + a glowing cross).
        if (eq.bare_head) {
            orphrey();
            cross(0.62f, 0.15f, 0.05f, BoneColor::Accent);
            buckle(m, f, 0.26f, 0.044f, BoneColor::Accent, false);
            return;
        }
        const f32 base = f.hc.y + f.hr.y * 0.62f;
        piece(m, BonePart::Head, Vec3{0.0f, base, 0.0f}, Vec3{f.hr.x * 2.2f, 0.05f, f.hr.z * 2.1f},
              BoneColor::Accent, BoneShape::Cylinder); // gold base band
        piece(m, BonePart::Head, Vec3{0.0f, base + f.hr.y * 0.75f, f.hr.z * 0.32f},
              Vec3{f.hr.x * 1.9f, f.hr.y * 1.6f, 0.05f}, BoneColor::Primary, BoneShape::Box, pitch(0.36f));
        piece(m, BonePart::Head, Vec3{0.0f, base + f.hr.y * 0.75f, -f.hr.z * 0.32f},
              Vec3{f.hr.x * 1.9f, f.hr.y * 1.6f, 0.05f}, BoneColor::Primary, BoneShape::Box, pitch(-0.36f));
        piece(m, BonePart::Head, Vec3{0.0f, base + f.hr.y * 0.55f, f.hr.z * 0.48f},
              Vec3{0.06f, f.hr.y * 1.3f, 0.04f}, BoneColor::Accent, BoneShape::Box, pitch(0.36f)); // gilt orphrey
        piece(m, BonePart::Head, Vec3{0.0f, base + f.hr.y * 0.8f, f.hr.z * 0.62f}, Vec3{0.022f, f.hr.y * 0.5f, 0.02f},
              BoneColor::Glow, BoneShape::Box, pitch(0.36f)); // cross vertical
        piece(m, BonePart::Head, Vec3{0.0f, base + f.hr.y * 0.92f, f.hr.z * 0.58f}, Vec3{f.hr.x * 0.6f, 0.022f, 0.02f},
              BoneColor::Glow, BoneShape::Box, pitch(0.36f)); // cross arms
        orphrey();
        cross(0.62f, 0.15f, 0.05f, BoneColor::Accent);
        buckle(m, f, 0.26f, 0.044f, BoneColor::Accent, false);
    }
}

// ------------------------------------------------------------------------------------------------
// Peasant - generic NPC townsfolk (the tunic, hose, leg wraps, shoes and headwear are the skinned
// OutfitMesh): a belt pouch, and a bundle or tool on some.
void build_peasant(CharacterModel& m, const Equipment& eq) {
    const Fit f(m);
    const u8 head = static_cast<u8>((eq.outfit_tint / 4u) % 4u);
    if (head != 3u) {
        hide_hair(m, f, head != 2u); // a hood / coif covers the crown; a straw hat just the tall hair
    }
    buckle(m, f, 0.22f, 0.036f, BoneColor::Dark, true);
}

// ------------------------------------------------------------------------------------------------
// Brigand - a MELEE bandit/cutthroat: (the hood, dagged jerkin, trousers + boots are skinned) a cloth
// mask over the nose + mouth (the classic bandit read), a crossed bandolier + a hip satchel, ONE
// scavenged iron pauldron, arm wraps. Grimy + asymmetric = a scruffy brigand, not a soldier.
void build_brigand(CharacterModel& m, const Equipment& eq) {
    (void)eq;
    const Fit f(m);
    hide_hair(m, f, true);
    piece(m, BonePart::Head, Vec3{0.0f, f.hc.y - f.hr.y * 0.4f, f.hr.z * 0.68f},
          Vec3{f.hr.x * 1.7f, f.hr.y * 0.75f, f.hr.z * 0.9f}, BoneColor::Primary); // the mask
    buckle(m, f, 0.2f, 0.05f, BoneColor::Metal, false);
    const Vec3 fr = f.front(0.6f, 0.04f);
    piece(m, BonePart::Torso, fr, Vec3{0.06f, f.span * 0.95f, 0.02f}, BoneColor::Dark,
          BoneShape::Box, roll(-0.6f)); // bandolier
    const TorsoRing r = f.at(0.12f);
    piece(m, BonePart::Pelvis, Vec3{-(r.rx + 0.05f), f.py(0.12f) - 0.04f, r.dz + 0.02f}, Vec3{0.08f, 0.12f, 0.12f},
          BoneColor::Dark); // hip satchel
    {
        const f32 o = outward(m, BonePart::UpperArmL);
        piece(m, BonePart::UpperArmL, Vec3{o * f.ar * 0.4f, f.ar * 0.2f, 0.0f},
              Vec3{f.ar * 3.6f, f.ar * 2.0f, f.ar * 3.7f}, BoneColor::Metal, BoneShape::Box, roll(-o * 0.3f));
        piece(m, BonePart::UpperArmL, Vec3{o * f.ar * 0.55f, -f.ar * 1.1f, 0.0f},
              Vec3{f.ar * 3.2f, f.ar * 1.1f, f.ar * 3.3f}, BoneColor::Metal, BoneShape::Box, roll(-o * 0.22f));
    }
    for (BonePart lo : {BonePart::LowerArmL, BonePart::LowerArmR}) {
        piece(m, lo, Vec3{0.0f, -f.seg(lo) * 0.55f, 0.0f}, Vec3{f.ar * 1.9f, f.seg(lo) * 0.7f, f.ar * 2.0f},
              BoneColor::Dark, BoneShape::Box); // arm wraps
    }
}

// ------------------------------------------------------------------------------------------------
// Outlaw - a RANGED bandit/poacher: (the deep hood + liripipe, jerkin, trousers + boots are skinned) a
// scarf over the lower face, bracers and a QUIVER of arrows across the back - so a ranged raider reads
// distinctly from the melee cutthroat even before drawing the bow.
void build_outlaw(CharacterModel& m, const Equipment& eq) {
    (void)eq;
    const Fit f(m);
    hide_hair(m, f, true);
    piece(m, BonePart::Head, Vec3{0.0f, f.hc.y - f.hr.y * 0.42f, f.hr.z * 0.66f},
          Vec3{f.hr.x * 1.66f, f.hr.y * 0.72f, f.hr.z * 0.9f}, BoneColor::Primary); // scarf
    buckle(m, f, 0.2f, 0.046f, BoneColor::Metal, true);
    for (BonePart lo : {BonePart::LowerArmL, BonePart::LowerArmR}) {
        piece(m, lo, Vec3{0.0f, -f.seg(lo) * 0.55f, 0.0f}, Vec3{f.ar * 1.9f, f.seg(lo) * 0.66f, f.ar * 2.0f},
              BoneColor::Dark, BoneShape::Box); // bracers
    }
    quiver(m, f, -1.0f, BoneColor::Accent);
}

} // namespace

void apply_outfit(CharacterModel& model, OutfitKind kind, const Equipment& equip) {
    CharacterPalette& pal = model.palette();

    if (kind == OutfitKind::Peasant) {
        // Earthy homespun, with per-NPC variety from the (re-purposed) tint index: bits 0-1 pick the
        // tunic, 2-3 the headwear (hood / linen coif / straw hat / bare), the rest the hood's dye.
        static const Vec3 tunics[4] = {{0.54f, 0.42f, 0.28f},  // undyed tan wool
                                       {0.40f, 0.43f, 0.30f},  // olive
                                       {0.38f, 0.30f, 0.24f},  // russet brown
                                       {0.36f, 0.40f, 0.46f}}; // faded woad blue
        static const Vec3 hoods[4] = {{0.52f, 0.20f, 0.14f},  // madder red
                                      {0.62f, 0.48f, 0.22f},  // weld yellow-ochre
                                      {0.26f, 0.34f, 0.50f},  // woad blue
                                      {0.30f, 0.38f, 0.22f}}; // green
        pal.primary = tunics[equip.outfit_tint % 4];
        pal.pants = Vec3{0.30f, 0.25f, 0.19f};
        pal.dark = Vec3{0.24f, 0.17f, 0.11f};                          // belt / purse
        pal.accent = hoods[(equip.outfit_tint / 16u + equip.outfit_tint) % 4]; // the hood
        pal.shirt = pal.primary;
        build_peasant(model, equip);
        return;
    }

    if (kind == OutfitKind::Brigand || kind == OutfitKind::Outlaw) {
        // Grimy scavenged bandit garb: dark worn leather, tarnished mismatched iron, a drab cloth
        // mask/scarf. A little per-bandit variety via the (re-purposed) tint index.
        static const Vec3 brigand_cloth[4] = {{0.46f, 0.20f, 0.18f},  // dried-blood red rag
                                              {0.40f, 0.36f, 0.30f},  // dirty grey
                                              {0.34f, 0.26f, 0.18f},  // mud brown
                                              {0.30f, 0.30f, 0.34f}}; // slate
        static const Vec3 outlaw_cloth[4] = {{0.30f, 0.36f, 0.22f},  // moss green
                                             {0.42f, 0.34f, 0.22f},  // tan
                                             {0.26f, 0.30f, 0.24f},  // dark olive
                                             {0.36f, 0.28f, 0.20f}}; // umber
        const bool ranged = kind == OutfitKind::Outlaw;
        pal.primary = (ranged ? outlaw_cloth : brigand_cloth)[equip.outfit_tint % 4];
        pal.dark = ranged ? Vec3{0.16f, 0.14f, 0.11f} : Vec3{0.14f, 0.11f, 0.09f}; // near-black leather
        pal.metal = Vec3{0.34f, 0.33f, 0.34f};                                     // tarnished dull iron
        pal.accent = Vec3{0.44f, 0.36f, 0.20f};                                    // dull brass buckles
        pal.shirt = Vec3{0.18f, 0.16f, 0.15f};                                     // dark under-tunic
        pal.pants = ranged ? Vec3{0.21f, 0.19f, 0.15f} : Vec3{0.17f, 0.15f, 0.14f};
        pal.glow = Vec3{0.5f, 0.85f, 1.0f};
        if (ranged) {
            build_outlaw(model, equip);
        } else {
            build_brigand(model, equip);
        }
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
    // reads as a dark under-tunic rather than the random per-seed shirt/pants colour. Hose are a dyed
    // wool (a brown for the woodsman, a deep grey-blue else).
    pal.shirt = Vec3{0.20f, 0.18f, 0.16f};
    pal.pants = (kind == OutfitKind::Leather) ? Vec3{0.27f, 0.22f, 0.15f} : Vec3{0.18f, 0.18f, 0.21f};

    switch (kind) {
        case OutfitKind::Plate: build_plate(model, equip); break;
        case OutfitKind::Robe: build_robe(model, equip); break;
        case OutfitKind::Leather: build_leather(model, equip); break;
        case OutfitKind::Holy: build_holy(model, equip); break;
        case OutfitKind::Peasant:                 // handled above (early return)
        case OutfitKind::Brigand:                 // handled above (early return)
        case OutfitKind::Outlaw: break;           // handled above (early return)
    }
}

} // namespace alryn
