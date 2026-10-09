// Side quests - the notice board's work besides the wagon contracts (see Game/SideQuest.h for the
// kinds + tuning). Implemented as GameServer methods, like Contracts.cpp, so a quest's foes live in
// the same hostile list (ambush_) the combat pass already fights, and its rewards land in the party's
// wallet + every hero's XP. The board refreshes when the party reaches a new town; one quest runs at a
// time (alongside any haul); its camp / pack / chest / petals are only placed once a hero comes near.
#include <Alryn/Net/GameServer.h>

#include <Alryn/Core/Density.h>
#include <Alryn/Core/Log.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/ScatterHash.h>
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/World/Village.h>

#include <algorithm>
#include <cmath>

namespace alryn {

namespace {
// The ground under (x,z), honouring any digs + craters (the density), else the base heightfield.
f32 ground_y(const DensitySampler& density, f32 x, f32 z, u32 seed) {
    if (const auto g = raycast_density(density, Vec3{x, 60.0f, z}, Vec3{0.0f, -1.0f, 0.0f}, 120.0f)) {
        return g->y;
    }
    return worldgen::height(x, z, seed);
}
} // namespace

Vec3 GameServer::notice_board(const worldgen::Village& town, u32 seed) {
    // Every town lays out a notice board at the plaza's edge (World/Village.h); the quests are pinned there.
    for (const PropInstance& p : cached_village_props(town, seed)) {
        if (p.category == PropCategory::Decor && p.variant == kDecorNoticeBoard) {
            return p.position;
        }
    }
    const Vec2 at = town.center + Vec2{detail::kMarketHalf + 3.0f, 0.0f}; // (a town too crowded for one)
    return Vec3{at.x, worldgen::height(at.x, at.y, seed), at.y};
}

const GameServer::QuestRun* GameServer::active_quest() const {
    for (const QuestRun& q : quests_) {
        if (q.phase == QuestPhase::Active) {
            return &q;
        }
    }
    return nullptr;
}

bool GameServer::debug_accept_quest(u32 id) {
    if (active_quest() != nullptr) {
        return false;
    }
    for (QuestRun& q : quests_) {
        if (q.id == id && q.phase == QuestPhase::Offered) {
            q.phase = QuestPhase::Active;
            std::erase_if(quests_, [](const QuestRun& o) { return o.phase == QuestPhase::Offered; });
            return true;
        }
    }
    return false;
}

void GameServer::debug_spawn_bestiary(const Vec3& at, f32 yaw, std::span<const u8> only, bool frozen) {
    static constexpr u8 kAll[] = {0u, 1u, 2u, 3u, kEnemyShield, kEnemyHealer, kEnemySapper, kEnemyWarlord, kEnemyWolf,
                                  kEnemyAlpha};
    const std::span<const u8> kKinds = only.empty() ? std::span<const u8>(kAll) : only;
    const DensitySampler density = sampler_.as_sampler();
    for (usize i = 0; i < kKinds.size(); ++i) {
        Enemy e;
        e.id = next_ambush_id_++;
        e.kind = kKinds[i];
        e.health = enemy_max_health(e.kind);
        e.quest = 0xFFFFFFF0u; // a guard of no quest: holds its spot, fights whoever comes near
        // A few: a line abreast ahead of `at`, facing back toward it. The whole roster: a ring round it.
        Vec2 p;
        f32 face = frozen ? yaw : yaw + Pi;
        if (kKinds.size() <= 5) {
            const Vec2 fwd{std::cos(yaw), std::sin(yaw)};
            const Vec2 side{-fwd.y, fwd.x};
            p = Vec2{at.x, at.z} + fwd * (frozen ? 2.4f : 3.6f) +
                side * ((static_cast<f32>(i) - 0.5f * static_cast<f32>(kKinds.size() - 1)) * 2.0f);
        } else {
            const f32 a = yaw + TwoPi * static_cast<f32>(i) / static_cast<f32>(kKinds.size());
            p = Vec2{at.x, at.z} + Vec2{std::cos(a), std::sin(a)} * 3.6f;
            face = a + Pi;
        }
        e.position = Vec3{p.x, ground_y(density, p.x, p.y, sampler_.seed()), p.y};
        e.home = e.position;
        e.yaw = face;
        if (frozen) {
            e.stagger = 1e6f; // reeling forever: no step, no swing - a statue to look at
        }
        ambush_.push_back(e);
    }
}

// A site out in the wilds for a quest posted in `town`: well clear of any town, on dry, walkable land,
// off the roads (a camp beside the highway would be a haul's ambush, not a quest). Deterministic per
// `salt` so the same offer always points at the same place. nullopt if the land round about is hopeless.
std::optional<Vec3> GameServer::quest_site(const worldgen::Village& town, u32 salt) const {
    const u32 seed = sampler_.seed();
    // Prefer OPEN ground - plains, bog, uplands, dry scrub - where a fight can be seen and fought;
    // failing that, the least wooded (driest) spot found.
    std::optional<Vec3> best;
    f32 best_moist = 1e9f;
    for (int attempt = 0; attempt < 40; ++attempt) {
        const u32 h = detail::tree_hash(static_cast<int>(salt), attempt, 8813u);
        const f32 ang = detail::hash01(h) * TwoPi;
        const f32 r = kQuestSiteMin + (kQuestSiteMax - kQuestSiteMin) * detail::hash01(h ^ 0x9E37u);
        const Vec2 p = town.center + Vec2{std::cos(ang), std::sin(ang)} * r;
        const f32 y = worldgen::height(p.x, p.y, seed);
        if (y < worldgen::water_level + 1.2f || worldgen::inside_village(p.x, p.y, seed, 24.0f) ||
            roads::distance(p.x, p.y, seed) < 10.0f) {
            continue;
        }
        // Gentle ground for a camp / a fight (no cliff faces).
        const f32 slope = std::max(std::abs(worldgen::height(p.x + 3.0f, p.y, seed) - worldgen::height(p.x - 3.0f, p.y, seed)),
                                   std::abs(worldgen::height(p.x, p.y + 3.0f, seed) - worldgen::height(p.x, p.y - 3.0f, seed)));
        if (slope > 2.6f) {
            continue;
        }
        const worldgen::Biome biome = worldgen::biome_at(p.x, p.y, seed);
        const f32 moist = worldgen::moisture(p.x, p.y, seed);
        const bool open = moist < -0.1f || biome == worldgen::Biome::Plains || biome == worldgen::Biome::Bog ||
                          biome == worldgen::Biome::Mountains || biome == worldgen::Biome::Snow ||
                          biome == worldgen::Biome::Desert;
        if (open) {
            return Vec3{p.x, y, p.y};
        }
        if (moist < best_moist) {
            best_moist = moist;
            best = Vec3{p.x, y, p.y};
        }
    }
    return best;
}

void GameServer::generate_quests(const worldgen::Village& town) {
    std::erase_if(quests_, [](const QuestRun& q) { return q.phase == QuestPhase::Offered; });
    quest_town_vseed_ = town.vseed;
    const u32 seed = sampler_.seed();
    const Vec3 board = notice_board(town, seed);
    // A green party isn't sent after the deadliest camps: the board's danger is capped by the best level.
    const u8 max_danger = progression_ ? std::clamp<u8>(max_danger_for_level(party_level()), 1, 3) : 3;
    const u32 h0 = detail::tree_hash(static_cast<int>(town.vseed), static_cast<int>(quest_round_), 8800u);
    for (u8 k = 0; k < kQuestOffers; ++k) {
        // Three different kinds on a board, rotating with every quest done in this town.
        const auto kind = static_cast<QuestKind>((h0 + k) % kQuestKinds);
        const u32 salt = detail::tree_hash(static_cast<int>(town.vseed), static_cast<int>(quest_round_ * 8u + k), 8801u);
        const auto site = quest_site(town, salt);
        if (!site) {
            continue;
        }
        QuestRun q;
        q.id = salt | 1u;
        q.kind = kind;
        q.phase = QuestPhase::Offered;
        q.danger = static_cast<u8>(1u + detail::tree_hash(static_cast<int>(salt), 3, 8802u) % max_danger);
        q.goal = quest_goal(kind, q.danger);
        q.reward = quest_reward(kind, q.danger);
        q.xp = quest_xp(kind, q.danger);
        q.site = *site;
        q.board = board;
        quests_.push_back(q);
    }
    ALRYN_INFO("Town {} posts {} side quest(s) on its notice board", town.vseed, quests_.size());
}

// The party has come near: place what the quest is about - the camp's raiders (with their warlord),
// the wolf pack (with its alpha), the buried chest under its X, the moonpetals (and the wolves denned
// among them). Foes stand at posts around the site, which is the home they guard.
void GameServer::wake_quest(QuestRun& q, const DensitySampler& density) {
    q.woken = true;
    const u32 seed = sampler_.seed();
    auto spawn = [&](u8 kind, f32 ang, f32 rad) {
        Enemy e;
        e.id = next_ambush_id_++;
        e.kind = kind;
        e.health = enemy_max_health(kind);
        e.quest = q.id;
        const f32 x = q.site.x + std::cos(ang) * rad;
        const f32 z = q.site.z + std::sin(ang) * rad;
        e.position = Vec3{x, ground_y(density, x, z, seed), z};
        e.home = e.position;
        e.yaw = ang; // facing out from the camp fire
        ambush_.push_back(e);
    };
    const u8 n = quest_foe_count(q.kind, q.danger);
    switch (q.kind) {
        case QuestKind::BanditCamp: {
            // The warlord holds the fire; the rest ring it - cutthroats, archers, a shield-bearer, a brute.
            static constexpr u8 kCrew[] = {0u, 3u, 0u, kEnemyShield, 3u, 2u, 0u, kEnemyHealer};
            spawn(kEnemyWarlord, 0.0f, 1.5f);
            for (u8 i = 1; i < n; ++i) {
                const f32 ang = TwoPi * static_cast<f32>(i) / static_cast<f32>(n - 1) + 0.4f;
                spawn(kCrew[(i - 1u) % std::size(kCrew)], ang, 4.0f + 2.5f * static_cast<f32>(i % 2));
            }
            break;
        }
        case QuestKind::WolfHunt:
            spawn(kEnemyAlpha, 0.0f, 0.8f);
            for (u8 i = 1; i < n; ++i) {
                spawn(kEnemyWolf, TwoPi * static_cast<f32>(i) / static_cast<f32>(n - 1), 3.5f + 1.5f * static_cast<f32>(i % 2));
            }
            break;
        case QuestKind::Treasure:
            quest_items_.push_back({next_quest_item_++, q.id, q.site, 1u, 0u}); // the chest, buried under the X
            break;
        case QuestKind::Herbs: {
            for (u8 i = 0; i < kHerbCount; ++i) {
                Vec3 at = q.site;
                for (int attempt = 0; attempt < 8; ++attempt) {
                    const u32 h = detail::tree_hash(static_cast<int>(q.id), i * 16 + attempt, 8820u);
                    const f32 a = detail::hash01(h) * TwoPi;
                    const f32 r = 3.0f + (kHerbScatter - 3.0f) * detail::hash01(h ^ 0x51u);
                    const f32 x = q.site.x + std::cos(a) * r, z = q.site.z + std::sin(a) * r;
                    if (worldgen::height(x, z, seed) > worldgen::water_level + 0.5f) {
                        at = Vec3{x, ground_y(density, x, z, seed), z};
                        break;
                    }
                }
                quest_items_.push_back({next_quest_item_++, q.id, at, 0u, 0u});
            }
            for (u8 i = 0; i < n; ++i) {
                spawn(kEnemyWolf, Pi * static_cast<f32>(i) + 0.7f, 5.0f);
            }
            break;
        }
    }
    ALRYN_INFO("Side quest '{}' - the party draws near", quest_title(q.kind));
}

void GameServer::finish_quest(QuestRun& q) {
    q.phase = QuestPhase::Complete;
    q.progress = q.goal;
    q.banner = kQuestBannerSeconds;
    money_ += q.reward;
    for (auto& [id, pl] : players_) {
        award_xp(pl, q.xp);
    }
    ++quest_round_; // the next board in this town offers something fresh
    ALRYN_INFO("Side quest complete: {} (+{} gold, +{} xp each)", quest_title(q.kind), q.reward, q.xp);
}

void GameServer::quest_foe_felled(u32 quest) {
    for (QuestRun& q : quests_) {
        if (q.id != quest || q.phase != QuestPhase::Active) {
            continue;
        }
        // Only the camp's raiders / the hunted pack ARE the goal; grave-robbers + the meadow's wolves are
        // just in the way.
        if (q.kind == QuestKind::BanditCamp || q.kind == QuestKind::WolfHunt) {
            q.progress = static_cast<u8>(std::min<u32>(q.progress + 1u, q.goal));
            if (q.progress >= q.goal) {
                finish_quest(q);
            }
        }
    }
}

void GameServer::quest_dig(const Vec3& at) {
    for (QuestRun& q : quests_) {
        if (q.kind != QuestKind::Treasure || q.phase != QuestPhase::Active || !q.woken ||
            glm::length(Vec2{at.x - q.site.x, at.z - q.site.z}) > kTreasureDigReach) {
            continue;
        }
        for (QuestItem& it : quest_items_) {
            if (it.quest != q.id || it.kind != 1u || it.state != 0u) {
                continue;
            }
            q.progress = static_cast<u8>(std::min<u32>(q.progress + 1u, q.goal));
            if (q.progress >= q.goal) {
                // The spade rings on wood: the chest is turned up at the bottom of the hole - and the
                // grave-robbers who were watching come running.
                const DensitySampler density = sampler_.as_sampler();
                it.state = 1u;
                it.position.y = ground_y(density, it.position.x, it.position.z, sampler_.seed());
                for (u8 i = 0; i < kTreasureRobbers; ++i) {
                    Enemy e;
                    e.id = next_ambush_id_++;
                    e.kind = i == 0 ? 0u : 3u;
                    e.health = enemy_max_health(e.kind);
                    e.quest = q.id;
                    const f32 ang = detail::hash01(detail::tree_hash(static_cast<int>(q.id), i, 8830u)) * TwoPi;
                    const f32 x = q.site.x + std::cos(ang) * 16.0f, z = q.site.z + std::sin(ang) * 16.0f;
                    e.position = Vec3{x, ground_y(density, x, z, sampler_.seed()), z};
                    e.home = q.site;
                    ambush_.push_back(e);
                }
            }
        }
    }
}

void GameServer::update_quests(Timestep dt, const DensitySampler& density) {
    if (players_.empty()) {
        return;
    }
    const u32 seed = sampler_.seed();
    QuestRun* active = nullptr;
    for (QuestRun& q : quests_) {
        if (q.phase == QuestPhase::Active) {
            active = &q;
        }
    }

    // The board: re-pin offers when the party reaches a new town (and once a quest is done or given up -
    // both clear quest_town_vseed_), unless one is under way. A town with no fit site round about just
    // posts nothing (it isn't retried every tick).
    Vec3 centroid{0.0f};
    for (const auto& [id, pl] : players_) {
        centroid += pl.controller.position();
    }
    centroid /= static_cast<f32>(players_.size());
    if (active == nullptr) {
        if (const auto town = worldgen::village_containing(centroid.x, centroid.z, seed, 16.0f);
            town && town->vseed != quest_town_vseed_) {
            generate_quests(*town);
        }
    }

    // A pick at the board takes the quest (the other offers come down); an abandon drops the one under way.
    for (auto& [id, pl] : players_) {
        if (pl.input.quest_pick != 0 && active == nullptr) {
            for (QuestRun& q : quests_) {
                if (q.id == pl.input.quest_pick && q.phase == QuestPhase::Offered &&
                    glm::length(Vec2{pl.controller.position().x - q.board.x, pl.controller.position().z - q.board.z}) <
                        kQuestBoardRange + 2.0f) {
                    const u32 taken = q.id;
                    q.phase = QuestPhase::Active;
                    std::erase_if(quests_, [](const QuestRun& o) { return o.phase == QuestPhase::Offered; });
                    for (QuestRun& r : quests_) {
                        if (r.id == taken) {
                            active = &r;
                        }
                    }
                    ALRYN_INFO("Side quest taken: {}", quest_title(active->kind));
                    break;
                }
            }
        }
        if (pl.input.quest_abandon && active != nullptr) {
            const u32 gone = active->id;
            std::erase_if(ambush_, [gone](const Enemy& e) { return e.quest == gone; });
            std::erase_if(quest_items_, [gone](const QuestItem& it) { return it.quest == gone; });
            std::erase_if(quests_, [gone](const QuestRun& q) { return q.id == gone; });
            active = nullptr;
            ++quest_round_;
            quest_town_vseed_ = 0; // re-pin the board
            ALRYN_INFO("Side quest abandoned");
        }
    }

    if (active != nullptr) {
        QuestRun& q = *active;
        if (!q.woken) {
            for (const auto& [id, pl] : players_) {
                if (glm::length(pl.controller.position() - q.site) < kQuestWakeRange) {
                    wake_quest(q, density);
                    break;
                }
            }
        }
        // E at the site: crack the unearthed chest open, or pick a moonpetal.
        for (auto& [id, pl] : players_) {
            if (!pl.input.grab || q.phase != QuestPhase::Active) {
                continue;
            }
            const Vec3 p = pl.controller.position();
            for (QuestItem& it : quest_items_) {
                if (it.quest != q.id) {
                    continue;
                }
                const f32 d = glm::length(Vec2{p.x - it.position.x, p.z - it.position.z});
                if (it.kind == 1u && it.state == 1u && d < kChestOpenRange) {
                    it.state = 2u; // opened - the gold is the party's
                    finish_quest(q);
                    break;
                }
                if (it.kind == 0u && it.state == 0u && d < kHerbPickRange) {
                    it.state = 1u; // picked
                    q.progress = static_cast<u8>(std::min<u32>(q.progress + 1u, q.goal));
                    if (q.progress >= q.goal) {
                        finish_quest(q);
                    }
                    break;
                }
            }
        }
        std::erase_if(quest_items_, [](const QuestItem& it) { return it.kind == 0u && it.state != 0u; });
        // A woken camp / pack that's somehow gone without being felled (a debug clear) still counts.
        if (q.phase == QuestPhase::Active && q.woken &&
            (q.kind == QuestKind::BanditCamp || q.kind == QuestKind::WolfHunt) &&
            std::none_of(ambush_.begin(), ambush_.end(), [&](const Enemy& e) { return e.quest == q.id; })) {
            finish_quest(q);
        }
    }

    // A finished quest's banner runs out: it comes off the board (the opened chest with it). Anything of
    // it still prowling (the meadow's wolves) slinks off once nobody's about.
    for (QuestRun& q : quests_) {
        if (q.phase == QuestPhase::Complete) {
            q.banner -= dt.seconds;
        }
    }
    for (const QuestRun& q : quests_) {
        if (q.phase == QuestPhase::Complete && q.banner <= 0.0f) {
            const u32 done = q.id;
            std::erase_if(quest_items_, [done](const QuestItem& it) { return it.quest == done; });
            quest_town_vseed_ = 0; // re-pin the board
        }
    }
    std::erase_if(quests_, [](const QuestRun& q) { return q.phase == QuestPhase::Complete && q.banner <= 0.0f; });
    std::erase_if(ambush_, [&](const Enemy& e) {
        if (e.quest == 0 || errand_owns(e.quest) ||
            std::any_of(quests_.begin(), quests_.end(), [&](const QuestRun& q) { return q.id == e.quest; })) {
            return false;
        }
        for (const auto& [id, pl] : players_) {
            if (glm::length(pl.controller.position() - e.position) < 60.0f) {
                return false;
            }
        }
        return true;
    });
}

} // namespace alryn
