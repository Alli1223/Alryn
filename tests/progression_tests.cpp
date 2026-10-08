#include <doctest/doctest.h>

#include <Alryn/Game/Progression.h>
#include <Alryn/Net/ByteBuffer.h>
#include <Alryn/Net/GameServer.h>
#include <Alryn/Net/NetClient.h>
#include <Alryn/Net/Protocol.h>

#include <bit>
#include <memory>
#include <string>
#include <vector>
#include <string>

using namespace alryn;
using namespace alryn::net;

TEST_CASE("Progression: the XP curve climbs monotonically to the level cap") {
    CHECK(level_for_xp(0) == 1);
    CHECK(xp_to_reach(1) == 0u);
    CHECK(xp_to_reach(2) == xp_step(1));
    for (u8 l = 2; l <= kMaxLevel; ++l) {
        CHECK(xp_to_reach(l) > xp_to_reach(static_cast<u8>(l - 1)));
        CHECK(level_for_xp(xp_to_reach(l)) == l);              // exactly enough reaches the level
        CHECK(level_for_xp(xp_to_reach(l) - 1u) == l - 1u);    // one short does not
    }
    CHECK(level_for_xp(max_xp() * 3u) == kMaxLevel);           // the cap holds
    CHECK(level_progress(0) == doctest::Approx(0.0f));
    CHECK(level_progress(xp_step(1) / 2u) == doctest::Approx(0.5f));
    CHECK(level_progress(max_xp()) == doctest::Approx(1.0f));
    // Levelling is felt: level 1 is the old balance, higher levels are tougher + hit harder.
    CHECK(level_health_mult(1) == doctest::Approx(1.0f));
    CHECK(level_damage_mult(1) == doctest::Approx(1.0f));
    CHECK(level_health_mult(kMaxLevel) > 1.3f);
    CHECK(level_damage_mult(kMaxLevel) > 1.2f);
    // Tougher raiders are worth more; a long dangerous haul pays more XP than a short safe one.
    CHECK(enemy_xp(2) > enemy_xp(0));
    CHECK(delivery_xp(3, 600.0f) > delivery_xp(1, 150.0f));
}

TEST_CASE("Progression: every role's skill tree is a well-formed four-tier tree") {
    for (u8 r = 0; r < kRoleCount; ++r) {
        const auto role = static_cast<PlayerRole>(r);
        // Exactly two starter skills - a fresh hero has a small, basic kit.
        CHECK(std::popcount(starter_mask(role)) == 2);
        int per_tier[kSkillTiers] = {};
        for (u8 a = 0; a < kAbilityCount; ++a) {
            const SkillNode n = skill_node(role, a);
            REQUIRE(n.tier < kSkillTiers);
            ++per_tier[n.tier];
            CHECK(n.req_level == kTierLevel[n.tier]);
            if (n.prereq >= 0) {
                // A parent is always a real ability on a LOWER tier (no cycles, no dangling links).
                REQUIRE(n.prereq < static_cast<i8>(kAbilityCount));
                CHECK(skill_node(role, static_cast<u8>(n.prereq)).tier < n.tier);
            } else {
                CHECK(n.tier <= 1); // only the roots + the Mage's second elements stand alone
            }
        }
        CHECK(per_tier[0] == 2);
        CHECK(per_tier[3] == 1); // one capstone
    }
}

TEST_CASE("Progression: learning is gated by level, parent node and skill points") {
    const PlayerRole k = PlayerRole::Knight;
    const u8 start = starter_mask(k);
    // Level 1: no points yet, so nothing can be learned.
    CHECK_FALSE(can_learn(k, 1, 1, start, 0));
    // Level 2: one point -> Bulwark (tier 1, parent Shield Bash) is learnable...
    CHECK(can_learn(k, 1, 2, start, 0));
    // ...but Rally (tier 2) needs level 4 AND Bulwark first.
    CHECK_FALSE(can_learn(k, 5, 2, start, 0));
    CHECK_FALSE(can_learn(k, 5, 4, start, 0));                            // no Bulwark yet
    CHECK(can_learn(k, 5, 4, static_cast<u8>(start | (1u << 1)), 0));     // with Bulwark: yes
    // A known skill can't be learned twice.
    CHECK_FALSE(can_learn(k, 0, 5, start, 0));
    // Points run out: at level 2 (1 point) with Bulwark learned there's nothing left.
    CHECK(points_available(k, 2, static_cast<u8>(start | (1u << 1)), 0) == 0);
    CHECK_FALSE(can_learn(k, 4, 2, static_cast<u8>(start | (1u << 1)), 0));

    // Talents: rank-gated by level, spend the same points.
    CHECK(can_raise_talent(k, 0, 2, start, 0));
    const u8 t1 = with_talent_rank(0, 0, 1);
    CHECK(talent_rank(t1, 0) == 1);
    CHECK_FALSE(can_raise_talent(k, 0, 3, start, t1)); // rank 2 needs level 5
    CHECK(can_raise_talent(k, 0, 5, start, t1));
    CHECK(talent_health_mult(with_talent_rank(0, 0, 3)) == doctest::Approx(1.24f));
    CHECK(talent_cooldown_mult(with_talent_rank(0, 2, 2)) < 1.0f);
    CHECK(points_spent(k, start, with_talent_rank(t1, 1, 2)) == 3);

    // A saved loadout is honoured only if it could have been earned at that level.
    CHECK(loadout_valid(k, 4, static_cast<u8>((1u << 1) | (1u << 5)), 0));   // Bulwark + Rally at 4
    CHECK_FALSE(loadout_valid(k, 3, static_cast<u8>((1u << 1) | (1u << 5)), 0)); // Rally needs 4
    CHECK_FALSE(loadout_valid(k, 2, static_cast<u8>((1u << 1) | (1u << 4)), 0)); // 2 skills, 1 point
    CHECK_FALSE(loadout_valid(k, 6, static_cast<u8>(1u << 6), 0));               // Leap without Rally
}

TEST_CASE("Progression: a Mage only weaves the elements + combos it has learned") {
    const u8 start = starter_mask(PlayerRole::Mage); // Fire + Water
    CHECK(spell_for_combo_known(1, 0, 0, 0, start) == SpellId::Fireball);
    CHECK(spell_for_combo_known(0, 1, 0, 0, start) == SpellId::FrostBolt);
    CHECK(spell_for_combo_known(2, 0, 0, 0, start) == SpellId::Fireball);  // no Meteor learned yet
    CHECK(spell_for_combo_known(0, 0, 3, 0, start) == SpellId::None);      // no Earth at all
    const u8 earth_wall = static_cast<u8>(start | (1u << 2) | (1u << 4));
    CHECK(spell_for_combo_known(0, 0, 3, 0, earth_wall) == SpellId::RockWall);
    CHECK(spell_required_ability(SpellId::Meteor) == 5);
    CHECK(spell_required_ability(SpellId::HealBloom) == 3);
}

TEST_CASE("Progression: the journey walks a hero through the game in order") {
    HeroRecord r;
    CHECK_FALSE(journey_met(0, r));
    r.read_contract = true;
    CHECK(journey_met(static_cast<u8>(JourneyGoal::ReadContract), r));
    CHECK_FALSE(journey_met(static_cast<u8>(JourneyGoal::SetOut), r));
    r.deliveries = 1; // a delivery also implies the earlier steps
    CHECK(journey_met(static_cast<u8>(JourneyGoal::SetOut), r));
    CHECK(journey_met(static_cast<u8>(JourneyGoal::Deliver), r));
    r.kills = 3;
    CHECK(journey_count(static_cast<u8>(JourneyGoal::DefeatRaiders), r) == 3);
    CHECK_FALSE(journey_met(static_cast<u8>(JourneyGoal::DefeatRaiders), r));
    r.kills = kJourneyRaiders;
    CHECK(journey_met(static_cast<u8>(JourneyGoal::DefeatRaiders), r));
    r.best_danger = 2;
    CHECK(journey_met(static_cast<u8>(JourneyGoal::Danger2), r));
    CHECK_FALSE(journey_met(static_cast<u8>(JourneyGoal::Danger3), r));
    CHECK_FALSE(journey_met(kJourneySteps, r)); // "Done" never completes
    for (u8 s = 0; s <= kJourneySteps; ++s) {
        CHECK(journey_step(s).title[0] != '\0');
    }
    // Contract danger opens up with level.
    CHECK(max_danger_for_level(1) == 1);
    CHECK(max_danger_for_level(3) == 2);
    CHECK(max_danger_for_level(kMaxLevel) == 3);
}

TEST_CASE("Progression: names, colours + hero progress survive the wire") {
    PlayerInput in;
    in.name = "Aldric the Bold";
    in.color_pref = 5;
    in.learn = 9;
    in.lantern = true;
    in.restore = true;
    in.progress = HeroProgress{1234u, 0x12u, 0x05u, 4u, 2u, 0x0021u, 17u, 3u, 2u};
    ByteWriter w;
    write(w, in);
    ByteReader rd(w.bytes(), w.size());
    PlayerInput out;
    REQUIRE(read(rd, out));
    CHECK(out.name == "Aldric the Bold");
    CHECK(out.color_pref == 5);
    CHECK(out.learn == 9);
    CHECK(out.lantern);
    CHECK(out.restore);
    CHECK(out.progress == in.progress);

    // A non-restoring input doesn't carry the progress block (inputs stay small).
    PlayerInput plain;
    ByteWriter w2;
    write(w2, plain);
    CHECK(w2.size() < w.size() - 10u);

    Snapshot s;
    s.players.push_back({});
    s.players[0].id = 3;
    s.players[0].color = 6;
    s.players[0].level = 7;
    s.players[0].lantern = 1;
    s.players[0].name = "Wren";
    s.players[0].progress.xp = 2222u;
    s.players[0].progress.journey = 9u;
    ByteWriter ws;
    write(ws, s);
    ByteReader rs(ws.bytes(), ws.size());
    Snapshot back;
    REQUIRE(read(rs, back));
    REQUIRE(back.players.size() == 1);
    CHECK(back.players[0].color == 6);
    CHECK(back.players[0].level == 7);
    CHECK(back.players[0].lantern == 1);
    CHECK(back.players[0].name == "Wren");
    CHECK(back.players[0].progress.xp == 2222u);
    CHECK(back.players[0].progress.journey == 9u);
}

namespace {
// A tiny harness: a GameServer + N connected clients, pumped one tick at a time.
struct Party {
    GameServer server;
    std::vector<std::unique_ptr<NetClient>> clients;
    std::vector<PlayerId> ids;
    std::vector<PlayerInput> inputs;
    std::vector<Snapshot> snaps;
    bool ok = false;

    Party(u16 port, u32 seed, int n) {
        if (!server.start(port, seed)) {
            return;
        }
        for (int i = 0; i < n; ++i) {
            clients.push_back(std::make_unique<NetClient>());
            if (!clients.back()->connect("127.0.0.1", port)) {
                return;
            }
        }
        ids.assign(n, 0);
        inputs.assign(n, PlayerInput{});
        snaps.assign(n, Snapshot{});
        ok = true;
    }
    void pump(int ticks) {
        for (int t = 0; t < ticks; ++t) {
            for (usize i = 0; i < clients.size(); ++i) {
                if (ids[i] != 0) {
                    clients[i]->send_input(inputs[i]);
                }
            }
            server.tick(Timestep{1.0f / 60.0f});
            for (usize i = 0; i < clients.size(); ++i) {
                for (const ClientEvent& e : clients[i]->poll(1)) {
                    if (e.type == ClientEventType::WelcomeReceived) {
                        ids[i] = e.welcome.your_id;
                    } else if (e.type == ClientEventType::SnapshotReceived) {
                        snaps[i] = e.snapshot;
                    }
                }
            }
        }
    }
};
} // namespace

TEST_CASE("GameServer: every player gets their own colour, a clean name and their preferred colour") {
    Party p(24721, 777u, 2);
    if (!p.ok) {
        MESSAGE("Could not bind game server - skipping");
        return;
    }
    p.inputs[0].name = "  Mira <3 the Swift!!  ";
    p.inputs[1].name = "";
    p.pump(120);
    REQUIRE(p.ids[0] != 0);
    REQUIRE(p.ids[1] != 0);
    const auto& a = p.server.players().at(p.ids[0]);
    const auto& b = p.server.players().at(p.ids[1]);
    CHECK(a.color != b.color);                  // distinct identity colours
    CHECK(a.name == "Mira 3 the Swift");        // sanitised + trimmed
    CHECK(b.name.rfind("PLAYER", 0) == 0);      // a nameless player still gets a label
    REQUIRE(p.snaps[1].players.size() == 2);    // and everyone sees everyone's name + colour
    bool saw_mira = false;
    for (const PlayerState& ps : p.snaps[1].players) {
        if (ps.id == p.ids[0]) {
            saw_mira = ps.name == a.name && ps.color == a.color;
        }
    }
    CHECK(saw_mira);

    // B asks for A's colour: refused (taken). Then asks for a free one: granted.
    p.inputs[1].color_pref = a.color;
    p.pump(4);
    CHECK(p.server.players().at(p.ids[1]).color != a.color);
    u8 free_color = 0;
    while (free_color == a.color || free_color == b.color) {
        ++free_color;
    }
    p.inputs[1].color_pref = free_color;
    p.pump(4);
    CHECK(p.server.players().at(p.ids[1]).color == free_color);
}

TEST_CASE("GameServer: with progression on, skills must be learned before they can be cast") {
    Party p(24722, 4242u, 1);
    if (!p.ok) {
        MESSAGE("Could not bind game server - skipping");
        return;
    }
    p.server.set_progression(true);
    PlayerInput& in = p.inputs[0];
    in.role = static_cast<u8>(PlayerRole::Cleric);
    p.pump(120);
    REQUIRE(p.ids[0] != 0);
    const PlayerId id = p.ids[0];
    const u8 aegis = 3; // tier 2: needs level 4 + Heal

    // Unlearned: casting Aegis does nothing.
    in.ability = static_cast<u8>(aegis + 1);
    p.pump(3);
    in.ability = 0;
    CHECK(p.server.players().at(id).shield_hp == doctest::Approx(0.0f));
    // Too low level to learn it either.
    in.learn = static_cast<u8>(aegis + 1);
    p.pump(3);
    in.learn = 0;
    p.pump(2);
    CHECK_FALSE(knows(p.server.players().at(id).known_mask(), aegis));

    // Level up to 4 (3 points), learn Aegis (one rising edge = one point), then cast it.
    p.server.grant_xp(id, xp_to_reach(4));
    p.pump(2);
    CHECK(p.server.players().at(id).level == 4);
    in.learn = static_cast<u8>(aegis + 1);
    p.pump(3);
    in.learn = 0;
    p.pump(2);
    CHECK(knows(p.server.players().at(id).known_mask(), aegis));
    CHECK(points_available(PlayerRole::Cleric, 4, p.server.players().at(id).known_mask(),
                           p.server.players().at(id).talent_mask()) == 2);
    in.ability = static_cast<u8>(aegis + 1);
    p.pump(3);
    in.ability = 0;
    CHECK(p.server.players().at(id).shield_hp > 0.0f); // learned -> it works

    // A talent: Vitality raises the health pool.
    const f32 hp_before = p.server.players().at(id).max_health;
    in.learn = 8; // talent 0 (Vitality)
    p.pump(3);
    in.learn = 0;
    p.pump(2);
    CHECK(p.server.players().at(id).max_health > hp_before * 1.05f);
    // The snapshot carries the owner's live progression (for the HUD + their saved hero).
    REQUIRE_FALSE(p.snaps[0].players.empty());
    CHECK(p.snaps[0].players[0].level == 4);
    CHECK(knows(p.snaps[0].players[0].progress.known, aegis));
    CHECK(talent_rank(p.snaps[0].players[0].progress.talents, 0) == 1);
}

TEST_CASE("GameServer: a saved hero is restored on joining (and a forged one is reset)") {
    Party p(24723, 777u, 2);
    if (!p.ok) {
        MESSAGE("Could not bind game server - skipping");
        return;
    }
    // Client A: a genuine level-5 Hunter with Volley + Caltrops learned, Might 1, journey step 4.
    PlayerInput& a = p.inputs[0];
    a.role = static_cast<u8>(PlayerRole::Hunter);
    a.restore = true;
    a.progress.xp = xp_to_reach(5) + 10u;
    a.progress.known = static_cast<u8>((1u << 1) | (1u << 5));
    a.progress.talents = with_talent_rank(0, 1, 1);
    a.progress.journey = 4;
    a.progress.owned_tier = 2;
    a.progress.kills = 12;
    a.progress.deliveries = 2;
    a.progress.best_danger = 2;
    // Client B: claims level 2 but a tier-3 capstone - not earnable, so the skills are reset.
    PlayerInput& b = p.inputs[1];
    b.role = static_cast<u8>(PlayerRole::Knight);
    b.restore = true;
    b.progress.xp = xp_to_reach(2);
    b.progress.known = static_cast<u8>(1u << 6);
    p.pump(120);
    REQUIRE(p.ids[0] != 0);
    REQUIRE(p.ids[1] != 0);
    const auto& ha = p.server.players().at(p.ids[0]);
    CHECK(ha.level == 5);
    CHECK(knows(ha.known_mask(), 1));
    CHECK(knows(ha.known_mask(), 5));
    CHECK(talent_rank(ha.talent_mask(), 1) == 1);
    CHECK(ha.owned_tier == 2);
    CHECK(ha.kills >= 12);
    CHECK(ha.journey >= 4); // restored (and maybe advanced: they already own gear)
    const auto& hb = p.server.players().at(p.ids[1]);
    CHECK(hb.level == 2);
    CHECK_FALSE(knows(hb.known_mask(), 6)); // forged capstone dropped
    CHECK(hb.known_mask() == starter_mask(PlayerRole::Knight));
}

TEST_CASE("GameServer: XP is shared for raiders felled nearby and the journey pays out") {
    Party p(24724, 4242u, 1);
    if (!p.ok) {
        MESSAGE("Could not bind game server - skipping");
        return;
    }
    p.pump(120);
    REQUIRE(p.ids[0] != 0);
    const PlayerId id = p.ids[0];
    const u32 xp0 = p.server.players().at(id).xp;
    const u8 step0 = p.server.players().at(id).journey;
    CHECK(step0 <= static_cast<u8>(JourneyGoal::SetOut)); // a fresh hero starts at the beginning
    // Walk to an offered wagon: the "find work" step completes and pays its reward.
    if (p.snaps[0].wagons.empty()) {
        MESSAGE("no contract offers in this town - skipping the journey half");
        return;
    }
    p.server.debug_place_player(id, p.snaps[0].wagons.front().position + Vec3{1.5f, 0.0f, 0.0f});
    p.pump(6);
    CHECK(p.server.players().at(id).journey > static_cast<u8>(JourneyGoal::ReadContract));
    CHECK(p.server.players().at(id).xp >= xp0 + journey_step(0).xp);
}

TEST_CASE("GameServer: a Knight holding a lantern out has stowed the shield (no block)") {
    GameServer::ServerPlayer p;
    p.role = PlayerRole::Knight;
    p.input.block = true;
    const f32 guarded = p.mitigated(100.0f);
    p.input.lantern = true; // the lantern takes the shield hand
    const f32 lantern = p.mitigated(100.0f);
    CHECK(lantern > guarded + 30.0f); // the block's big reduction is gone...
    p.input.block = false;
    CHECK(p.mitigated(100.0f) == doctest::Approx(lantern)); // ...it's just an unguarded Knight
}
