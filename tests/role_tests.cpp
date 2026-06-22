#include <doctest/doctest.h>

#include <Alryn/Game/Roles.h>

#include <string_view>

using namespace alryn;

TEST_CASE("role stats express the tank/damage/healer fantasy") {
    const RoleStats knight = role_stats(PlayerRole::Knight);
    const RoleStats hunter = role_stats(PlayerRole::Hunter);
    const RoleStats cleric = role_stats(PlayerRole::Cleric);

    // The tank is the toughest; the hunter the frailest.
    CHECK(knight.max_health > cleric.max_health);
    CHECK(cleric.max_health > hunter.max_health);
    // Only the tank mitigates incoming damage.
    CHECK(knight.damage_reduction > hunter.damage_reduction);
    // The hunter out-damages everyone at range; the knight hits hardest up close.
    CHECK(hunter.ranged_damage > knight.ranged_damage);
    CHECK(knight.melee_damage > hunter.melee_damage);
    // The hunter is the fleetest of foot.
    CHECK(hunter.move_speed > knight.move_speed);
}

TEST_CASE("every role has three named, cooldown-gated abilities") {
    for (u8 r = 0; r < kRoleCount; ++r) {
        const auto role = static_cast<PlayerRole>(r);
        CHECK(role_name(role)[0] != '\0');
        for (u8 slot = 0; slot < kAbilitySlots; ++slot) {
            const AbilityDef ab = ability_def(role, slot);
            CHECK(ab.name[0] != '\0');
            CHECK(ab.cooldown > 0.0f);
        }
    }
}

TEST_CASE("knight consecration is the third ability and burns over its life") {
    CHECK(std::string_view(ability_def(PlayerRole::Knight, 2).name) == "CONSECRATION");
    CHECK(kConsecrationRadius > 0.0f);
    CHECK(kConsecrationDuration > 0.0f);
    CHECK(kConsecrationDPS > 0.0f);                          // it hurts enemies
    CHECK(kConsecrationDPS < kHealAuraRate * 2.0f);          // but it's a *small* amount
}

TEST_CASE("every role now has four abilities, incl. the cleric Aegis shield") {
    for (u8 r = 0; r < kRoleCount; ++r) {
        for (u8 s = 0; s < kAbilitySlots; ++s) {
            CHECK(ability_def(static_cast<PlayerRole>(r), s).name[0] != '\0');
        }
    }
    CHECK(kAbilitySlots == 4);
    CHECK(std::string_view(ability_def(PlayerRole::Cleric, 3).name) == "AEGIS");
    CHECK(kAegisAmount > 0.0f);   // it absorbs damage
    CHECK(kAegisDuration > 0.0f); // and lasts a while
    CHECK(kAegisRange > 0.0f);
}

TEST_CASE("aura props table drives radius/duration/colour/light for each kind") {
    const AuraProps heal = aura_props(AuraKind::Heal);
    const AuraProps con = aura_props(AuraKind::Consecration);
    CHECK(heal.radius == doctest::Approx(kHealAuraRadius));
    CHECK(heal.duration == doctest::Approx(kHealAuraDuration));
    CHECK(con.radius == doctest::Approx(kConsecrationRadius));
    CHECK(heal.light > 0.0f); // both auras light up at night
    CHECK(con.light > 0.0f);
}

TEST_CASE("the skills tree expands each role beyond the four hotbar slots") {
    // More skills exist than the player can equip, so the action bar is a real choice.
    CHECK(kAbilityCount > kAbilitySlots);
    for (u8 r = 0; r < kRoleCount; ++r) {
        for (u8 a = 0; a < kAbilityCount; ++a) {
            const AbilityDef ab = ability_def(static_cast<PlayerRole>(r), a);
            CHECK(ab.name[0] != '\0');
            CHECK(ab.desc[0] != '\0'); // every skill carries a tree description
            CHECK(ab.cooldown > 0.0f);
        }
    }
    CHECK(std::string_view(ability_def(PlayerRole::Knight, 4).name) == "WHIRLWIND");
    CHECK(std::string_view(ability_def(PlayerRole::Hunter, 5).name) == "CALTROPS");
    CHECK(std::string_view(ability_def(PlayerRole::Cleric, 4).name) == "RENEW");
}

TEST_CASE("the hunter caltrops hazard aura wounds enemies (no taunt)") {
    const AuraProps hz = aura_props(AuraKind::Hazard);
    CHECK(hz.radius == doctest::Approx(kHazardRadius));
    CHECK(hz.duration == doctest::Approx(kHazardDuration));
    CHECK(hz.light > 0.0f);
    CHECK(kHazardDPS > 0.0f);
}

TEST_CASE("cleric channelled heal aura tuning is sane") {
    CHECK(kHealChargeTime > 0.0f);   // it takes time to charge
    CHECK(kHealAuraDuration > 0.0f); // and lingers
    CHECK(kHealAuraRadius > 0.0f);
    CHECK(kHealAuraRate > 0.0f);     // and actually heals
    // Over its lifetime the aura restores a meaningful chunk of health.
    CHECK(kHealAuraRate * kHealAuraDuration > 50.0f);
}

TEST_CASE("mitigation soaks damage by the role + block fraction") {
    // Pure-formula mirror of GameServer::ServerPlayer::mitigated (no server needed).
    const f32 raw = 100.0f;
    const RoleStats knight = role_stats(PlayerRole::Knight);
    const f32 taken = raw * (1.0f - knight.damage_reduction);
    CHECK(taken < raw);
    // Bulwark stacks on top, but mitigation is capped below 1 (never fully immune).
    const f32 blocked = raw * (1.0f - (knight.damage_reduction + kBulwarkReduction));
    CHECK(blocked < taken);
    CHECK(blocked > 0.0f);
}
