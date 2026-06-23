#include <doctest/doctest.h>

#include <Alryn/Game/Roles.h>
#include <Alryn/Physics/Collider.h>

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

TEST_CASE("the mage is a fourth, elemental combo-casting damage role") {
    CHECK(kRoleCount == 4);
    CHECK(std::string_view(role_name(PlayerRole::Mage)) == "MAGE");
    const RoleStats m = role_stats(PlayerRole::Mage);
    CHECK(m.ranged_damage > m.melee_damage); // a caster, not a brawler
    CHECK(m.max_health < role_stats(PlayerRole::Knight).max_health); // fragile

    // Combo recognition: the headline EARTH x3 -> rock wall, then dominant-element fallbacks.
    CHECK(spell_for_combo(0, 0, 3, 0) == SpellId::RockWall);
    CHECK(spell_for_combo(2, 0, 0, 0) == SpellId::Meteor);   // fire x2
    CHECK(spell_for_combo(1, 0, 0, 0) == SpellId::Fireball);
    CHECK(spell_for_combo(0, 1, 0, 0) == SpellId::FrostBolt);
    CHECK(spell_for_combo(0, 0, 1, 0) == SpellId::Boulder);
    CHECK(spell_for_combo(0, 0, 0, 1) == SpellId::HealBloom);
    CHECK(spell_for_combo(0, 0, 0, 0) == SpellId::None);
    for (int s = 1; s <= static_cast<int>(SpellId::RockWall); ++s) {
        CHECK(spell_cooldown(static_cast<SpellId>(s)) > 0.0f);
        CHECK(spell_name(static_cast<SpellId>(s))[0] != '\0');
    }
    CHECK(kRockWallHealth > 0.0f);
    CHECK(kRockWallTtl > 0.0f);
    CHECK(kRockWallLength > kRockWallThick); // a wall, not a pillar
}

TEST_CASE("co-op team abilities buff allies (empower / war horn / guardian leap)") {
    CHECK(kAbilityCount == 7); // a 7th, co-op ability per role
    CHECK(std::string_view(ability_def(PlayerRole::Knight, 6).name) == "GUARDIAN LEAP");
    CHECK(std::string_view(ability_def(PlayerRole::Hunter, 6).name) == "WAR HORN");
    CHECK(std::string_view(ability_def(PlayerRole::Cleric, 6).name) == "EMPOWER");
    // The Mage empowers allies with a double-nature combo (single nature still just heals).
    CHECK(spell_for_combo(0, 0, 0, 2) == SpellId::Empower);
    CHECK(spell_for_combo(0, 0, 0, 1) == SpellId::HealBloom);
    CHECK(kDamageBoostMult > 1.0f);      // empower actually boosts damage
    CHECK(kHasteMult > 1.0f);            // war horn actually speeds allies
    CHECK(kGuardShieldAmount > 0.0f);    // guardian leap shields the ally
    CHECK(kEmpowerRange > 0.0f);
    CHECK(kDamageBoostDuration > 0.0f);
}

TEST_CASE("a rock wall is a solid collider with clear ends (so NPCs route around it)") {
    // The same box collider GameServer::wall_colliders raises (a span along local x, thin in z).
    Collider c;
    c.shape = Collider::Shape::Box;
    c.center = Vec3{0.0f, 0.0f, 0.0f};
    c.half = Vec2{kRockWallLength * 0.5f, kRockWallThick * 0.5f};
    c.yaw = 0.0f;
    c.y_min = 0.0f;
    c.y_max = kRockWallHeight;

    // A walker standing in the wall's footprint is pushed out (the wall blocks the path).
    const Vec2 inside = resolve_collider(c, Vec2{0.0f, 0.0f}, 0.4f, 0.5f, 1.7f);
    CHECK(glm::length(inside) > 0.1f);
    // ...but a walker past the END of the span is untouched, so a route exists around it.
    const Vec2 beyond{kRockWallLength * 0.5f + 1.5f, 0.0f};
    const Vec2 around = resolve_collider(c, beyond, 0.4f, 0.5f, 1.7f);
    CHECK(glm::length(around - beyond) < 0.01f);
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
