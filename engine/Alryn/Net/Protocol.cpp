#include <Alryn/Net/Protocol.h>

namespace alryn::net {

namespace {
void write_appearance(ByteWriter& w, const CharacterAppearance& a) {
    w.write_u8(a.skin);
    w.write_u8(a.hair_color);
    w.write_u8(static_cast<u8>(a.eyes));
    w.write_u8(static_cast<u8>(a.ears));
    w.write_u8(static_cast<u8>(a.hair));
}
void read_appearance(ByteReader& r, CharacterAppearance& a) {
    a.skin = r.read_u8();
    a.hair_color = r.read_u8();
    a.eyes = static_cast<EyeStyle>(r.read_u8());
    a.ears = static_cast<EarStyle>(r.read_u8());
    a.hair = static_cast<HairStyle>(r.read_u8());
}
void write_equipment(ByteWriter& w, const Equipment& e) {
    w.write_u8(e.outfit_tier);
    w.write_u8(e.weapon_tier);
    w.write_u8(e.outfit_tint);
    w.write_u8(e.weapon_index);
}
void read_equipment(ByteReader& r, Equipment& e) {
    e.outfit_tier = r.read_u8();
    e.weapon_tier = r.read_u8();
    e.outfit_tint = r.read_u8();
    e.weapon_index = r.read_u8();
}
} // namespace

void write(ByteWriter& w, const PlayerInput& in) {
    w.write_u32(in.sequence);
    w.write_vec3(in.move);
    w.write_f32(in.yaw);
    w.write_f32(in.pitch);
    const u8 flags = static_cast<u8>((in.jump ? 1 : 0) | (in.dig ? 2 : 0) | (in.add ? 4 : 0) |
                                     (in.fire ? 8 : 0) | (in.attack ? 16 : 0) |
                                     (in.build ? 32 : 0) | (in.rally ? 64 : 0) |
                                     (in.grab ? 128 : 0));
    w.write_u8(flags);
    w.write_vec3(in.aim);
    w.write_u32(in.vote_wagon);
    w.write_u8(in.vote_mode);
    w.write_f32(in.throttle);
    w.write_f32(in.steer);
    w.write_u8(in.role);
    w.write_u8(in.ability);
    w.write_u8(in.spell);
    w.write_u8(in.block ? 1 : 0);
    write_appearance(w, in.appearance);
    write_equipment(w, in.equipment);
}

bool read(ByteReader& r, PlayerInput& in) {
    in.sequence = r.read_u32();
    in.move = r.read_vec3();
    in.yaw = r.read_f32();
    in.pitch = r.read_f32();
    const u8 flags = r.read_u8();
    in.jump = (flags & 1) != 0;
    in.dig = (flags & 2) != 0;
    in.add = (flags & 4) != 0;
    in.fire = (flags & 8) != 0;
    in.attack = (flags & 16) != 0;
    in.build = (flags & 32) != 0;
    in.rally = (flags & 64) != 0;
    in.grab = (flags & 128) != 0;
    in.aim = r.read_vec3();
    in.vote_wagon = r.read_u32();
    in.vote_mode = r.read_u8();
    in.throttle = r.read_f32();
    in.steer = r.read_f32();
    in.role = r.read_u8();
    in.ability = r.read_u8();
    in.spell = r.read_u8();
    in.block = r.read_u8() != 0;
    read_appearance(r, in.appearance);
    read_equipment(r, in.equipment);
    return r.ok();
}

void write(ByteWriter& w, const Snapshot& s) {
    w.write_u32(s.tick);
    w.write_f32(s.time_of_day);
    w.write_u8(s.weather);
    w.write_u8(s.outcome);
    w.write_u8(s.phase);
    w.write_f32(s.phase_timer);
    w.write_u8(s.wave);
    w.write_u8(s.houses_standing);
    w.write_u8(s.houses_total);
    w.write_u32(s.money);
    w.write_u8(s.contract_phase);
    w.write_u8(s.contract_outcome);
    w.write_u16(static_cast<u16>(s.players.size()));
    for (const PlayerState& p : s.players) {
        w.write_u32(p.id);
        w.write_vec3(p.position);
        w.write_f32(p.yaw);
        w.write_u8(p.health);
        w.write_u8(p.build_stock);
        w.write_u8(p.seated);
        w.write_u8(p.carrying);
        w.write_u8(p.role);
        w.write_u8(p.cast);
        w.write_u8(p.action);
        w.write_u8(p.shield);
        w.write_u8(p.buffs);
        write_appearance(w, p.appearance);
        write_equipment(w, p.equipment);
    }
    w.write_u16(static_cast<u16>(s.projectiles.size()));
    for (const ProjectileState& pr : s.projectiles) {
        w.write_vec3(pr.position);
        w.write_vec3(pr.dir);
        w.write_u8(pr.kind);
    }
    w.write_u16(static_cast<u16>(s.enemies.size()));
    for (const EnemyState& en : s.enemies) {
        w.write_u32(en.id);
        w.write_vec3(en.position);
        w.write_f32(en.yaw);
        w.write_u8(en.kind);
        w.write_u8(en.health);
        w.write_u8(en.action);
    }
    w.write_u16(static_cast<u16>(s.villagers.size()));
    for (const VillagerState& vl : s.villagers) {
        w.write_u32(vl.id);
        w.write_vec3(vl.position);
        w.write_f32(vl.yaw);
        w.write_u8(vl.health);
        w.write_u8(vl.kind);
        w.write_u8(vl.shield);
        write_appearance(w, vl.appearance);
    }
    w.write_u16(static_cast<u16>(s.fires.size()));
    for (const FireState& f : s.fires) {
        w.write_vec3(f.position);
        w.write_f32(f.yaw);
        w.write_u8(f.intensity);
    }
    w.write_u16(static_cast<u16>(s.barricades.size()));
    for (const BarricadeState& b : s.barricades) {
        w.write_vec3(b.position);
        w.write_f32(b.yaw);
        w.write_u8(b.health);
    }
    w.write_u16(static_cast<u16>(s.wagons.size()));
    for (const WagonState& wg : s.wagons) {
        w.write_u32(wg.id);
        w.write_vec3(wg.position);
        w.write_f32(wg.yaw);
        w.write_vec3(wg.dest);
        w.write_vec3(wg.source);
        w.write_vec3(wg.horse_pos);
        w.write_f32(wg.horse_yaw);
        w.write_u32(wg.reward);
        w.write_u8(wg.type);
        w.write_u8(wg.health);
        w.write_u8(wg.mode);
        w.write_u8(wg.difficulty);
        w.write_u8(wg.votes);
        w.write_u8(wg.has_horse);
        w.write_u8(wg.goods_aboard);
        w.write_u8(wg.goods_total);
        w.write_u8(wg.wheel_off);
        w.write_u8(wg.wheel_index);
        w.write_vec3(wg.wheel_pos);
        w.write_u8(wg.repair);
    }
    w.write_u16(static_cast<u16>(s.goods.size()));
    for (const GoodState& g : s.goods) {
        w.write_u32(g.id);
        w.write_vec3(g.position);
        w.write_u8(g.loose);
    }
    w.write_u16(static_cast<u16>(s.auras.size()));
    for (const AuraState& a : s.auras) {
        w.write_vec3(a.position);
        w.write_f32(a.radius);
        w.write_u8(a.kind);
    }
    w.write_u16(static_cast<u16>(s.walls.size()));
    for (const WallState& wl : s.walls) {
        w.write_vec3(wl.position);
        w.write_f32(wl.yaw);
        w.write_f32(wl.length);
        w.write_u8(wl.health);
    }
}

bool read(ByteReader& r, Snapshot& s) {
    s.tick = r.read_u32();
    s.time_of_day = r.read_f32();
    s.weather = r.read_u8();
    s.outcome = r.read_u8();
    s.phase = r.read_u8();
    s.phase_timer = r.read_f32();
    s.wave = r.read_u8();
    s.houses_standing = r.read_u8();
    s.houses_total = r.read_u8();
    s.money = r.read_u32();
    s.contract_phase = r.read_u8();
    s.contract_outcome = r.read_u8();
    const u16 count = r.read_u16();
    s.players.clear();
    s.players.reserve(count);
    for (u16 i = 0; i < count && r.ok(); ++i) {
        PlayerState p;
        p.id = r.read_u32();
        p.position = r.read_vec3();
        p.yaw = r.read_f32();
        p.health = r.read_u8();
        p.build_stock = r.read_u8();
        p.seated = r.read_u8();
        p.carrying = r.read_u8();
        p.role = r.read_u8();
        p.cast = r.read_u8();
        p.action = r.read_u8();
        p.shield = r.read_u8();
        p.buffs = r.read_u8();
        read_appearance(r, p.appearance);
        read_equipment(r, p.equipment);
        s.players.push_back(p);
    }
    const u16 proj_count = r.read_u16();
    s.projectiles.clear();
    s.projectiles.reserve(proj_count);
    for (u16 i = 0; i < proj_count && r.ok(); ++i) {
        ProjectileState pr;
        pr.position = r.read_vec3();
        pr.dir = r.read_vec3();
        pr.kind = r.read_u8();
        s.projectiles.push_back(pr);
    }
    const u16 enemy_count = r.read_u16();
    s.enemies.clear();
    s.enemies.reserve(enemy_count);
    for (u16 i = 0; i < enemy_count && r.ok(); ++i) {
        EnemyState en;
        en.id = r.read_u32();
        en.position = r.read_vec3();
        en.yaw = r.read_f32();
        en.kind = r.read_u8();
        en.health = r.read_u8();
        en.action = r.read_u8();
        s.enemies.push_back(en);
    }
    const u16 vill_count = r.read_u16();
    s.villagers.clear();
    s.villagers.reserve(vill_count);
    for (u16 i = 0; i < vill_count && r.ok(); ++i) {
        VillagerState vl;
        vl.id = r.read_u32();
        vl.position = r.read_vec3();
        vl.yaw = r.read_f32();
        vl.health = r.read_u8();
        vl.kind = r.read_u8();
        vl.shield = r.read_u8();
        read_appearance(r, vl.appearance);
        s.villagers.push_back(vl);
    }
    const u16 fire_count = r.read_u16();
    s.fires.clear();
    s.fires.reserve(fire_count);
    for (u16 i = 0; i < fire_count && r.ok(); ++i) {
        FireState f;
        f.position = r.read_vec3();
        f.yaw = r.read_f32();
        f.intensity = r.read_u8();
        s.fires.push_back(f);
    }
    const u16 barricade_count = r.read_u16();
    s.barricades.clear();
    s.barricades.reserve(barricade_count);
    for (u16 i = 0; i < barricade_count && r.ok(); ++i) {
        BarricadeState b;
        b.position = r.read_vec3();
        b.yaw = r.read_f32();
        b.health = r.read_u8();
        s.barricades.push_back(b);
    }
    const u16 wagon_count = r.read_u16();
    s.wagons.clear();
    s.wagons.reserve(wagon_count);
    for (u16 i = 0; i < wagon_count && r.ok(); ++i) {
        WagonState wg;
        wg.id = r.read_u32();
        wg.position = r.read_vec3();
        wg.yaw = r.read_f32();
        wg.dest = r.read_vec3();
        wg.source = r.read_vec3();
        wg.horse_pos = r.read_vec3();
        wg.horse_yaw = r.read_f32();
        wg.reward = r.read_u32();
        wg.type = r.read_u8();
        wg.health = r.read_u8();
        wg.mode = r.read_u8();
        wg.difficulty = r.read_u8();
        wg.votes = r.read_u8();
        wg.has_horse = r.read_u8();
        wg.goods_aboard = r.read_u8();
        wg.goods_total = r.read_u8();
        wg.wheel_off = r.read_u8();
        wg.wheel_index = r.read_u8();
        wg.wheel_pos = r.read_vec3();
        wg.repair = r.read_u8();
        s.wagons.push_back(wg);
    }
    const u16 good_count = r.read_u16();
    s.goods.clear();
    s.goods.reserve(good_count);
    for (u16 i = 0; i < good_count && r.ok(); ++i) {
        GoodState g;
        g.id = r.read_u32();
        g.position = r.read_vec3();
        g.loose = r.read_u8();
        s.goods.push_back(g);
    }
    const u16 aura_count = r.read_u16();
    s.auras.clear();
    s.auras.reserve(aura_count);
    for (u16 i = 0; i < aura_count && r.ok(); ++i) {
        AuraState a;
        a.position = r.read_vec3();
        a.radius = r.read_f32();
        a.kind = r.read_u8();
        s.auras.push_back(a);
    }
    const u16 wall_count = r.read_u16();
    s.walls.clear();
    s.walls.reserve(wall_count);
    for (u16 i = 0; i < wall_count && r.ok(); ++i) {
        WallState wl;
        wl.position = r.read_vec3();
        wl.yaw = r.read_f32();
        wl.length = r.read_f32();
        wl.health = r.read_u8();
        s.walls.push_back(wl);
    }
    return r.ok();
}

void write(ByteWriter& w, const Welcome& welcome) {
    w.write_u32(welcome.your_id);
    w.write_u32(welcome.seed);
}

bool read(ByteReader& r, Welcome& welcome) {
    welcome.your_id = r.read_u32();
    welcome.seed = r.read_u32();
    return r.ok();
}

void write(ByteWriter& w, const DeformEvent& deform) {
    w.write_vec3(deform.center);
    w.write_f32(deform.radius);
    w.write_f32(deform.amount);
}

bool read(ByteReader& r, DeformEvent& deform) {
    deform.center = r.read_vec3();
    deform.radius = r.read_f32();
    deform.amount = r.read_f32();
    return r.ok();
}

} // namespace alryn::net
