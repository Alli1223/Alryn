#include <Alryn/Character/Weapon.h>

namespace alryn {

namespace {

Mat4 T(const Vec3& t) { return glm::translate(Mat4{1.0f}, t); }
Mat4 S(const Vec3& s) { return glm::scale(Mat4{1.0f}, s); }
Mat4 Rx(f32 a) { return glm::rotate(Mat4{1.0f}, a, Vec3{1.0f, 0.0f, 0.0f}); }
Mat4 Ry(f32 a) { return glm::rotate(Mat4{1.0f}, a, Vec3{0.0f, 1.0f, 0.0f}); }

// How much fancier a weapon is at a tier (longer blade, brighter metal): 0.85 .. 1.15.
f32 grade(EquipmentTier t) { return 0.85f + 0.1f * static_cast<f32>(static_cast<u8>(t)); }

} // namespace

std::vector<WeaponPiece> weapon_pieces(WeaponType type, EquipmentTier tier, const CharacterPalette& p) {
    std::vector<WeaponPiece> w;
    const f32 g = grade(tier);
    const bool gilded = static_cast<u8>(tier) >= 2; // gold furniture at the higher tiers
    const Vec3 furniture = gilded ? p.accent : p.dark;

    switch (type) {
        case WeaponType::None:
            break;
        case WeaponType::Sword: {
            // Blade continues down the forearm (local -Y), tilted slightly forward.
            const Mat4 b = Rx(-0.35f);
            w.push_back({BoneShape::Box, b * S({0.30f, 0.05f, 0.07f}), furniture});            // crossguard
            w.push_back({BoneShape::Box, b * T({0, 0.12f, 0}) * S({0.05f, 0.22f, 0.05f}), p.dark}); // grip
            w.push_back({BoneShape::Sphere, b * T({0, 0.27f, 0}) * S({0.07f, 0.07f, 0.07f}), furniture}); // pommel
            w.push_back({BoneShape::Box, b * T({0, -0.6f * g, 0}) * S({0.07f, 1.1f * g, 0.16f}), p.metal}); // blade
            w.push_back({BoneShape::Box, b * T({0, -1.18f * g, 0}) * S({0.02f, 0.22f, 0.05f}), p.metal}); // tip
            break;
        }
        case WeaponType::Dagger: {
            const Mat4 b = Rx(-0.35f);
            w.push_back({BoneShape::Box, b * S({0.14f, 0.04f, 0.05f}), furniture});            // guard
            w.push_back({BoneShape::Box, b * T({0, 0.08f, 0}) * S({0.04f, 0.14f, 0.04f}), p.dark}); // grip
            w.push_back({BoneShape::Box, b * T({0, -0.26f, 0}) * S({0.05f, 0.5f, 0.1f}), p.metal}); // blade
            break;
        }
        case WeaponType::Bow: {
            // A vertical stave along the forearm axis, gripped at the middle, with recurved tips.
            const f32 h = 0.75f * g;
            w.push_back({BoneShape::Box, S({0.05f, 2.0f * h, 0.06f}), p.dark});                // stave
            for (f32 s : {1.0f, -1.0f}) {
                w.push_back({BoneShape::Box, T({0.07f, s * 0.9f * h, 0}) * S({0.05f, 0.3f * h, 0.05f}),
                             p.dark}); // recurved tip
            }
            w.push_back({BoneShape::Box, T({0.14f, 0, 0}) * S({0.012f, 1.9f * h, 0.012f}),
                         Vec3{0.85f, 0.82f, 0.74f}}); // bowstring
            if (gilded) {
                w.push_back({BoneShape::Box, T({0.01f, 0, 0}) * S({0.06f, 0.18f, 0.07f}), p.accent}); // grip wrap
            }
            break;
        }
        case WeaponType::Mace: {
            const Mat4 b = Rx(-0.2f);
            w.push_back({BoneShape::Box, b * T({0, -0.3f, 0}) * S({0.05f, 0.95f, 0.05f}), p.dark}); // haft
            w.push_back({BoneShape::Sphere, b * T({0, -0.82f, 0}) * S({0.17f * g, 0.18f * g, 0.17f * g}),
                         p.accent}); // gilded head
            for (int k = 0; k < 4; ++k) { // flanges
                const f32 a = static_cast<f32>(k) * HalfPi;
                w.push_back({BoneShape::Box,
                             b * T({0, -0.82f, 0}) * Ry(a) * T({0.14f, 0, 0}) * S({0.1f, 0.18f, 0.04f}),
                             p.accent});
            }
            w.push_back({BoneShape::Sphere, b * T({0, 0.14f, 0}) * S({0.06f, 0.06f, 0.06f}), p.accent}); // pommel
            break;
        }
        case WeaponType::Staff: {
            w.push_back({BoneShape::Box, T({0, -0.35f, 0}) * S({0.055f, 1.55f * g, 0.055f}), p.dark}); // shaft
            // A claw of prongs cradling the orb at the top.
            for (int k = 0; k < 3; ++k) {
                const f32 a = static_cast<f32>(k) * (TwoPi / 3.0f);
                w.push_back({BoneShape::Box,
                             T({0, 0.42f, 0}) * Ry(a) * T({0.08f, 0.02f, 0}) * Rx(0.5f) *
                                 S({0.03f, 0.16f, 0.03f}),
                             p.dark});
            }
            w.push_back({BoneShape::Sphere, T({0, 0.5f, 0}) * S({0.13f, 0.13f, 0.13f}), p.glow, true}); // orb
            for (int k = 0; k < 3; ++k) { // orbiting shards
                const f32 a = static_cast<f32>(k) * (TwoPi / 3.0f);
                w.push_back({BoneShape::Box,
                             T({0, 0.5f, 0}) * Ry(a) * T({0.22f, 0.05f, 0}) * S(Vec3{0.04f}), p.glow,
                             true});
            }
            break;
        }
        case WeaponType::Shield: {
            // Held in FRONT of the off forearm (local +Z): a heraldic field, a trim border, a boss,
            // and a cross emblem in the trim colour.
            const Vec3 trim = gilded ? p.accent : p.metal;
            w.push_back({BoneShape::RoundedBox, T({0, 0, 0.16f}) * S({0.66f, 0.82f, 0.08f}), trim}); // border
            w.push_back({BoneShape::RoundedBox, T({0, 0, 0.19f}) * S({0.56f, 0.72f, 0.10f}), p.primary}); // field
            w.push_back({BoneShape::Sphere, T({0, 0, 0.27f}) * S({0.11f, 0.11f, 0.11f}), trim}); // boss
            w.push_back({BoneShape::Box, T({0, 0.06f, 0.25f}) * S({0.09f, 0.4f, 0.03f}), trim}); // emblem |
            w.push_back({BoneShape::Box, T({0, 0.1f, 0.25f}) * S({0.32f, 0.09f, 0.03f}), trim});  // emblem -
            break;
        }
    }
    return w;
}

WeaponType role_weapon(u8 role, u8 weapon_index) {
    switch (role % 4u) {
        case 0: { // Knight - sword or mace
            static const WeaponType opts[] = {WeaponType::Sword, WeaponType::Mace};
            return opts[weapon_index % 2u];
        }
        case 1: // Hunter - bow
            return WeaponType::Bow;
        case 2: { // Cleric - mace or sword
            static const WeaponType opts[] = {WeaponType::Mace, WeaponType::Sword};
            return opts[weapon_index % 2u];
        }
        default: // Mage - staff
            return WeaponType::Staff;
    }
}

WeaponType role_offhand(u8 role) {
    switch (role % 4u) {
        case 0: return WeaponType::Shield; // Knight
        case 1: return WeaponType::Dagger; // Hunter
        case 2: return WeaponType::Shield; // Cleric
        default: return WeaponType::None;  // Mage (two-handed staff)
    }
}

u8 role_weapon_count(u8 role) {
    const u8 r = role % 4u;
    return (r == 0u || r == 2u) ? 2u : 1u;
}

const char* weapon_name(WeaponType type) {
    switch (type) {
        case WeaponType::Sword: return "SWORD";
        case WeaponType::Dagger: return "DAGGER";
        case WeaponType::Bow: return "BOW";
        case WeaponType::Staff: return "STAFF";
        case WeaponType::Mace: return "MACE";
        case WeaponType::Shield: return "SHIELD";
        default: return "NONE";
    }
}

} // namespace alryn
