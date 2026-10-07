#pragma once

// Saved HEROES: the player's characters, kept between sessions. A hero is who you are in the world
// (name, class, race + look, identity colour) and how far you've come (the progression the server
// tracks - XP, learned skills, talents, the journey, owned gear, ranks - plus the hotbar layout). The
// roster is a small text file in the user's data folder; the client restores the chosen hero into a
// server on joining (PlayerInput.restore) and saves its progress back as it grows.

#include <Alryn/Character/CharacterAppearance.h>
#include <Alryn/Game/Progression.h>
#include <Alryn/Game/Roles.h>
#include <Alryn/Net/Protocol.h>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace alryn::game {

struct Hero {
    std::string name = "HERO";
    PlayerRole role = PlayerRole::Knight;
    CharacterAppearance appearance;
    u8 outfit_tint = 0;   // index into outfit_tints()
    u8 weapon_index = 0;  // which of the role's weapons
    u8 color = 0;         // preferred identity colour (player_colors())
    net::HeroProgress progress;
    std::array<int, kAbilitySlots> bar{-1, -1, -1, -1}; // hotbar layout (ability index per slot)
    u32 played_seconds = 0;

    u8 level() const { return level_for_xp(progress.xp); }
    // The abilities this hero can use: their role's starter kit + what they've learned.
    u8 known_mask() const { return static_cast<u8>(progress.known | starter_mask(role)); }
};

// A fresh hero of `role`: the role's signature outfit colour, its starter skills on the hotbar.
Hero make_hero(PlayerRole role);
// Fills any empty hotbar slots with known abilities (and drops ones that aren't known).
void tidy_bar(Hero& hero);

struct Roster {
    std::vector<Hero> heroes;
    int selected = 0; // index into heroes (clamped on load)

    Hero* current() {
        return heroes.empty() ? nullptr : &heroes[static_cast<usize>(std::clamp(selected, 0, static_cast<int>(heroes.size()) - 1))];
    }
};
inline constexpr usize kMaxHeroes = 6;

// Where the roster lives: ALRYN_PROFILE_DIR if set, else the platform's per-user data folder
// (%APPDATA%/Alryn on Windows, $XDG_DATA_HOME/alryn or ~/.local/share/alryn elsewhere), falling back
// to the executable's folder.
std::filesystem::path roster_path();
Roster load_roster();
bool save_roster(const Roster& roster);

} // namespace alryn::game
