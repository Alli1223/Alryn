#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

#include <array>

namespace alryn {

// Player-chosen cosmetic options for a character. Compact (indices + small enums)
// so it serialises cheaply over the wire and resolves to colours/feature geometry
// via the shared palettes below and CharacterModel::create().
enum class EyeStyle : u8 { Round, Wide, Sleepy, Sharp };
enum class EarStyle : u8 { Round, Pointed, Small };
enum class HairStyle : u8 { Bald, Short, Spiky, Mohawk, Ponytail };
// The playable RACES. Men (Human) are the baseline; Dwarves are short + broad + bearded; Elves are
// tall + slender with long pointed ears. Race drives the model proportions + characteristic features
// (dwarf beard / elf ears), and rides in the networked appearance so everyone sees the right race.
enum class Race : u8 { Human, Dwarf, Elf };

constexpr u8 kEyeStyleCount = 4;
constexpr u8 kEarStyleCount = 3;
constexpr u8 kHairStyleCount = 5;
constexpr u8 kRaceCount = 3;

struct CharacterAppearance {
    u8 skin = 1;       // index into skin_tones()
    u8 hair_color = 1; // index into hair_colors()
    EyeStyle eyes = EyeStyle::Round;
    EarStyle ears = EarStyle::Round;
    HairStyle hair = HairStyle::Short;
    Race race = Race::Human; // kept LAST so existing aggregate initialisers stay valid

    bool operator==(const CharacterAppearance&) const = default;
};

// Per-race body proportions (scales applied on top of the seed's own variation) + the characteristic
// feature flags. Read by CharacterModel::generate/add_features, so changing these re-proportions the
// whole figure (body + outfit are skinned from the same bone metrics).
struct RaceTraits {
    f32 height = 1.0f; // overall height scale
    f32 build = 1.0f;  // width / bulk scale (broad dwarf, slim elf)
    f32 legs = 1.0f;   // leg-length scale (dwarves have short legs)
    f32 arms = 1.0f;   // arm-length scale
    f32 head = 1.0f;   // head-size scale
    bool long_ears = false; // long pointed elven ears
    bool beard = false;     // a full dwarven beard
};

inline RaceTraits race_traits(Race r) {
    switch (r) {
        case Race::Dwarf: return {0.82f, 1.20f, 0.78f, 1.0f, 1.08f, false, true};
        case Race::Elf: return {1.10f, 0.90f, 1.14f, 1.06f, 0.94f, true, false};
        case Race::Human: break;
    }
    return {}; // Men - the baseline
}

inline const char* race_name(Race r) {
    switch (r) {
        case Race::Dwarf: return "DWARF";
        case Race::Elf: return "ELF";
        case Race::Human: break;
    }
    return "MAN";
}

// Selectable skin tones (light -> dark). Shared by the UI swatches and the model.
inline const std::array<Vec3, 6>& skin_tones() {
    static const std::array<Vec3, 6> tones = {
        Vec3{0.96f, 0.81f, 0.71f}, Vec3{0.89f, 0.69f, 0.55f}, Vec3{0.80f, 0.58f, 0.45f},
        Vec3{0.64f, 0.45f, 0.34f}, Vec3{0.49f, 0.33f, 0.25f}, Vec3{0.34f, 0.23f, 0.18f}};
    return tones;
}

// Selectable hair colours.
inline const std::array<Vec3, 6>& hair_colors() {
    static const std::array<Vec3, 6> colors = {
        Vec3{0.08f, 0.07f, 0.08f}, // black
        Vec3{0.34f, 0.21f, 0.12f}, // brown
        Vec3{0.85f, 0.68f, 0.36f}, // blond
        Vec3{0.74f, 0.24f, 0.13f}, // ginger
        Vec3{0.80f, 0.82f, 0.86f}, // silver
        Vec3{0.45f, 0.28f, 0.68f}, // dyed violet
    };
    return colors;
}

inline Vec3 skin_color(u8 index) {
    const auto& t = skin_tones();
    return t[index % t.size()];
}
inline Vec3 hair_color_of(u8 index) {
    const auto& c = hair_colors();
    return c[index % c.size()];
}

} // namespace alryn
