#include "Profile.h"

#include <Alryn/Core/Log.h>
#include <Alryn/Core/Paths.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace alryn::game {

namespace {
// The role's signature outfit colour (an outfit_tints index), so a new hero of each class reads
// distinctly by default: blue Knight, green Hunter, white Cleric, violet Mage.
u8 signature_tint(PlayerRole r) {
    switch (r) {
        case PlayerRole::Knight: return 0;
        case PlayerRole::Hunter: return 2;
        case PlayerRole::Cleric: return 5;
        case PlayerRole::Mage: return 3;
    }
    return 0;
}

std::filesystem::path data_dir() {
    if (const char* d = std::getenv("ALRYN_PROFILE_DIR"); d != nullptr && d[0] != '\0') {
        return std::filesystem::path{d};
    }
#ifdef _WIN32
    if (const char* app = std::getenv("APPDATA"); app != nullptr && app[0] != '\0') {
        return std::filesystem::path{app} / "Alryn";
    }
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path{xdg} / "alryn";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path{home} / ".local" / "share" / "alryn";
    }
#endif
    return executable_dir();
}
} // namespace

Hero make_hero(PlayerRole role) {
    Hero h;
    h.role = role;
    h.outfit_tint = signature_tint(role);
    h.color = static_cast<u8>(static_cast<u8>(role) * 2u % kPlayerColorCount);
    tidy_bar(h);
    return h;
}

void tidy_bar(Hero& hero) {
    const u8 known = hero.known_mask();
    if (hero.role == PlayerRole::Mage) {
        hero.bar = {0, 1, 2, 3}; // the Mage's keys are its four elements (locked ones show a lock)
        return;
    }
    for (int& slot : hero.bar) {
        if (slot < 0 || slot >= static_cast<int>(kAbilityCount) || !knows(known, static_cast<u8>(slot))) {
            slot = -1;
        }
    }
    for (u8 a = 0; a < kAbilityCount; ++a) {
        if (!knows(known, a) || std::find(hero.bar.begin(), hero.bar.end(), static_cast<int>(a)) != hero.bar.end()) {
            continue;
        }
        const auto empty = std::find(hero.bar.begin(), hero.bar.end(), -1);
        if (empty == hero.bar.end()) {
            break;
        }
        *empty = static_cast<int>(a);
    }
}

std::filesystem::path roster_path() { return data_dir() / "heroes.txt"; }

Roster load_roster() {
    Roster roster;
    std::ifstream in(roster_path());
    if (!in) {
        return roster;
    }
    std::string line;
    Hero* h = nullptr;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "selected") {
            ss >> roster.selected;
        } else if (key == "hero") {
            roster.heroes.emplace_back();
            h = &roster.heroes.back();
        } else if (h == nullptr) {
            continue;
        } else if (key == "name") {
            std::string rest;
            std::getline(ss, rest);
            std::string name;
            for (const char c : rest) {
                if (name_char_ok(c) && name.size() < kMaxNameLength && !(c == ' ' && name.empty())) {
                    name.push_back(c);
                }
            }
            h->name = name.empty() ? std::string{"HERO"} : name;
        } else {
            // Everything else is a list of integers.
            std::vector<long long> v;
            long long x = 0;
            while (ss >> x) {
                v.push_back(x);
            }
            auto at = [&](usize i) { return i < v.size() ? v[i] : 0LL; };
            auto u8v = [&](usize i, u8 mod) { return static_cast<u8>(std::clamp<long long>(at(i), 0, 255) % mod); };
            if (key == "role") {
                h->role = static_cast<PlayerRole>(u8v(0, kRoleCount));
            } else if (key == "look") {
                h->appearance.skin = u8v(0, static_cast<u8>(skin_tones().size()));
                h->appearance.hair_color = u8v(1, static_cast<u8>(hair_colors().size()));
                h->appearance.eyes = static_cast<EyeStyle>(u8v(2, kEyeStyleCount));
                h->appearance.ears = static_cast<EarStyle>(u8v(3, kEarStyleCount));
                h->appearance.hair = static_cast<HairStyle>(u8v(4, kHairStyleCount));
                h->appearance.race = static_cast<Race>(u8v(5, kRaceCount));
            } else if (key == "tint") {
                h->outfit_tint = u8v(0, 8);
            } else if (key == "weapon") {
                h->weapon_index = u8v(0, 8);
            } else if (key == "color") {
                h->color = u8v(0, kPlayerColorCount);
            } else if (key == "xp") {
                h->progress.xp = static_cast<u32>(std::clamp<long long>(at(0), 0, max_xp()));
            } else if (key == "known") {
                h->progress.known = static_cast<u8>(at(0) & kAllAbilities);
            } else if (key == "talents") {
                h->progress.talents = static_cast<u8>(at(0) & 0x3F);
            } else if (key == "journey") {
                h->progress.journey = static_cast<u8>(std::clamp<long long>(at(0), 0, kJourneySteps));
            } else if (key == "tier") {
                h->progress.owned_tier = static_cast<u8>(std::clamp<long long>(at(0), 0, 3));
            } else if (key == "ranks") {
                h->progress.ranks = static_cast<u16>(at(0) & 0xFFFF);
            } else if (key == "kills") {
                h->progress.kills = static_cast<u16>(std::clamp<long long>(at(0), 0, 0xFFFF));
            } else if (key == "deliveries") {
                h->progress.deliveries = static_cast<u16>(std::clamp<long long>(at(0), 0, 0xFFFF));
            } else if (key == "danger") {
                h->progress.best_danger = static_cast<u8>(std::clamp<long long>(at(0), 0, 3));
            } else if (key == "bar") {
                for (usize i = 0; i < kAbilitySlots; ++i) {
                    h->bar[i] = static_cast<int>(std::clamp<long long>(i < v.size() ? v[i] : -1, -1, kAbilityCount - 1));
                }
            } else if (key == "played") {
                h->played_seconds = static_cast<u32>(std::max<long long>(at(0), 0));
            }
        }
    }
    for (Hero& hero : roster.heroes) {
        tidy_bar(hero);
    }
    if (roster.heroes.size() > kMaxHeroes) {
        roster.heroes.resize(kMaxHeroes);
    }
    roster.selected = roster.heroes.empty() ? 0 : std::clamp(roster.selected, 0, static_cast<int>(roster.heroes.size()) - 1);
    ALRYN_INFO("Loaded {} hero(es) from {}", roster.heroes.size(), roster_path().string());
    return roster;
}

bool save_roster(const Roster& roster) {
    const std::filesystem::path path = roster_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // Write to a temp file then swap it in, so a crash mid-save can't wipe the roster.
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            ALRYN_WARN("Could not save heroes to {}", path.string());
            return false;
        }
        out << "alryn-heroes 1\n";
        out << "selected " << roster.selected << "\n";
        for (const Hero& h : roster.heroes) {
            const auto& a = h.appearance;
            const auto& p = h.progress;
            out << "hero\n";
            out << "name " << h.name << "\n";
            out << "role " << static_cast<int>(h.role) << "\n";
            out << "look " << static_cast<int>(a.skin) << ' ' << static_cast<int>(a.hair_color) << ' '
                << static_cast<int>(a.eyes) << ' ' << static_cast<int>(a.ears) << ' ' << static_cast<int>(a.hair)
                << ' ' << static_cast<int>(a.race) << "\n";
            out << "tint " << static_cast<int>(h.outfit_tint) << "\n";
            out << "weapon " << static_cast<int>(h.weapon_index) << "\n";
            out << "color " << static_cast<int>(h.color) << "\n";
            out << "xp " << p.xp << "\n";
            out << "known " << static_cast<int>(p.known) << "\n";
            out << "talents " << static_cast<int>(p.talents) << "\n";
            out << "journey " << static_cast<int>(p.journey) << "\n";
            out << "tier " << static_cast<int>(p.owned_tier) << "\n";
            out << "ranks " << p.ranks << "\n";
            out << "kills " << p.kills << "\n";
            out << "deliveries " << p.deliveries << "\n";
            out << "danger " << static_cast<int>(p.best_danger) << "\n";
            out << "bar " << h.bar[0] << ' ' << h.bar[1] << ' ' << h.bar[2] << ' ' << h.bar[3] << "\n";
            out << "played " << h.played_seconds << "\n";
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // rename can't replace an existing file on some platforms - fall back to remove + rename.
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
    }
    return !ec;
}

} // namespace alryn::game
