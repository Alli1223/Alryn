// Character progression on the server (see Game/Progression.h): identity colours + names, the saved
// hero adopted on joining, XP + levels, skill-tree learning, talents and the journey. Kept in its own
// translation unit alongside Abilities.cpp / Contracts.cpp; the state lives on GameServer::ServerPlayer.

#include <Alryn/Core/Log.h>
#include <Alryn/Game/Progression.h>
#include <Alryn/Net/GameServer.h>

#include <algorithm>
#include <format>

namespace alryn {

namespace {
constexpr f32 kReadContractRange = 5.0f; // standing this close to an offered wagon "reads" its contract
} // namespace

void GameServer::assign_color(net::PlayerId id) {
    const auto it = players_.find(id);
    if (it == players_.end()) {
        return;
    }
    bool used[kPlayerColorCount] = {};
    for (const auto& [oid, other] : players_) {
        if (oid != id) {
            used[other.color % kPlayerColorCount] = true;
        }
    }
    // Start the search from the id so a lone rejoining player doesn't always come back red.
    for (u8 k = 0; k < kPlayerColorCount; ++k) {
        const u8 c = static_cast<u8>((id + k) % kPlayerColorCount);
        if (!used[c]) {
            it->second.color = c;
            return;
        }
    }
    it->second.color = static_cast<u8>(id % kPlayerColorCount); // more players than colours: share
}

void GameServer::sync_identity(net::PlayerId id, ServerPlayer& player) {
    // Name: printable characters only, clamped; an empty name falls back to "PLAYER <id>".
    std::string name;
    for (const char c : player.input.name) {
        if (name.size() >= kMaxNameLength) {
            break;
        }
        if (c == ' ' && (name.empty() || name.back() == ' ')) {
            continue; // no leading / doubled spaces
        }
        if (name_char_ok(c)) {
            name.push_back(c);
        }
    }
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    player.name = name.empty() ? std::format("PLAYER {}", id) : name;

    // Colour preference: honoured when no one else is wearing that colour.
    const u8 pref = player.input.color_pref;
    if (pref < kPlayerColorCount && pref != player.color) {
        bool taken = false;
        for (const auto& [oid, other] : players_) {
            if (oid != id && other.color == pref) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            player.color = pref;
        }
    }
}

void GameServer::restore_hero(ServerPlayer& player) {
    if (player.restored || !player.input.restore) {
        return;
    }
    player.restored = true;
    const net::HeroProgress& h = player.input.progress;
    const auto role = static_cast<PlayerRole>(player.input.role % kRoleCount);
    const u8 r = static_cast<u8>(role);
    // Keep anything already earned this session (normally nothing - the restore lands on join).
    player.xp = std::min(max_xp(), h.xp + player.xp);
    player.level = level_for_xp(player.xp);
    u8 known = static_cast<u8>(h.known & kAllAbilities);
    u8 talents = static_cast<u8>(h.talents & 0x3Fu);
    if (!loadout_valid(role, player.level, known, talents)) {
        ALRYN_WARN("Saved hero loadout doesn't add up for level {} - resetting their skills", player.level);
        known = 0;
        talents = 0;
    }
    player.known[r] = static_cast<u8>(known & ~starter_mask(role));
    player.talents[r] = talents;
    player.journey = std::min<u8>(h.journey, kJourneySteps);
    player.owned_tier = std::max<u8>(player.owned_tier, std::min<u8>(h.owned_tier, kTierCount - 1u));
    for (u8 a = 0; a < kAbilityCount; ++a) {
        const u8 rk = std::min<u8>(static_cast<u8>((h.ranks >> (2 * a)) & 0x3u), ability_max_rank(role, a));
        u8& slot = player.ability_rank[r * kAbilityCount + a];
        slot = std::max(slot, rk);
    }
    player.kills = std::max(player.kills, h.kills);
    player.deliveries = std::max(player.deliveries, h.deliveries);
    player.best_danger = std::max<u8>(player.best_danger, std::min<u8>(h.best_danger, 3u));
    ALRYN_INFO("Hero '{}' restored: level {} ({} xp), journey step {}", player.name, player.level,
               player.xp, player.journey);
}

void GameServer::award_xp(ServerPlayer& player, u32 xp) {
    if (xp == 0) {
        return;
    }
    const u8 before = player.level;
    player.xp = std::min(max_xp(), player.xp + xp);
    player.level = level_for_xp(player.xp);
    if (player.level > before) {
        // A level-up restores the hero to full (at the new, larger health pool).
        sync_player_role(player);
        player.health = player.max_health;
        ALRYN_INFO("Player '{}' reached level {}", player.name, player.level);
    }
}

void GameServer::grant_xp(net::PlayerId id, u32 xp) {
    const auto it = players_.find(id);
    if (it != players_.end()) {
        award_xp(it->second, xp);
    }
}

void GameServer::award_kill(const Vec3& where, u8 kind) {
    const u32 xp = enemy_xp(kind);
    for (auto& [id, pl] : players_) {
        if (glm::length(pl.controller.position() - where) <= kXpShareRadius) {
            award_xp(pl, xp);
            pl.kills = static_cast<u16>(std::min<u32>(pl.kills + 1u, 0xFFFFu));
        }
    }
}

void GameServer::award_delivery(u8 difficulty, f32 route_length) {
    const u32 xp = delivery_xp(difficulty, route_length);
    for (auto& [id, pl] : players_) {
        award_xp(pl, xp);
        pl.deliveries = static_cast<u16>(std::min<u32>(pl.deliveries + 1u, 0xFFFFu));
        pl.best_danger = std::max(pl.best_danger, difficulty);
    }
}

u8 GameServer::party_level() const {
    u8 best = 1;
    for (const auto& [id, pl] : players_) {
        best = std::max(best, pl.level);
    }
    return best;
}

void GameServer::update_progression(Timestep /*dt*/) {
    for (auto& [id, pl] : players_) {
        sync_identity(id, pl);
        const bool was_restored = pl.restored;
        restore_hero(pl);
        sync_player_role(pl); // the role, max health (level + Vitality) and walk speed for this tick
        pl.level = level_for_xp(pl.xp);
        if (!pl.spawned || (pl.restored && !was_restored)) {
            // A hero arrives whole: at their class + level's full health (a Knight used to spawn on
            // the generic 100 HP and stay that way until a haul's regen kicked in).
            pl.health = pl.max_health;
            pl.spawned = true;
        }

        // Skill tree: spend ONE skill point per rising edge of input.learn.
        const u8 learn = pl.input.learn;
        if (learn != 0 && pl.prev_learn == 0) {
            const u8 r = static_cast<u8>(pl.role);
            if (learn <= kAbilityCount) {
                const u8 a = static_cast<u8>(learn - 1);
                if (can_learn(pl.role, a, pl.level, pl.known_mask(), pl.talent_mask())) {
                    pl.known[r] = static_cast<u8>(pl.known[r] | (1u << a));
                }
            } else if (learn >= 8 && learn < 8 + kTalentCount) {
                const u8 t = static_cast<u8>(learn - 8);
                if (can_raise_talent(pl.role, t, pl.level, pl.known_mask(), pl.talent_mask())) {
                    pl.talents[r] = with_talent_rank(pl.talents[r], t,
                                                     static_cast<u8>(talent_rank(pl.talents[r], t) + 1));
                    sync_player_role(pl); // Vitality grows the health pool at once
                }
            }
        }
        pl.prev_learn = learn;

        // The journey: advance through every step this hero now satisfies, paying each reward.
        HeroRecord rec;
        rec.level = pl.level;
        rec.kills = pl.kills;
        rec.deliveries = pl.deliveries;
        rec.best_danger = pl.best_danger;
        rec.learned = points_spent(pl.role, pl.known_mask(), pl.talent_mask()) > 0;
        rec.geared = pl.owned_tier >= 1;
        for (u8 a = 0; a < kAbilityCount && !rec.honed; ++a) {
            rec.honed = pl.rank_of(a) >= 1;
        }
        rec.set_out = contract_phase_ == ContractPhase::Active;
        if (contract_phase_ == ContractPhase::Offer) {
            const Vec3 pp = pl.controller.position();
            for (const Wagon& w : offers_) {
                if (glm::length(Vec2{w.position.x - pp.x, w.position.z - pp.z}) < kReadContractRange) {
                    rec.read_contract = true;
                    break;
                }
            }
            rec.read_contract = rec.read_contract || pl.input.vote_wagon != 0;
        }
        while (pl.journey < kJourneySteps && journey_met(pl.journey, rec)) {
            const JourneyStep step = journey_step(pl.journey);
            ++pl.journey;
            award_xp(pl, step.xp);
            rec.level = pl.level;
            ALRYN_INFO("Player '{}' completed journey step '{}'", pl.name, step.title);
        }
    }
}

} // namespace alryn
