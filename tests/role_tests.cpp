#include <doctest/doctest.h>

#include <Alryn/Game/Roles.h>

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
