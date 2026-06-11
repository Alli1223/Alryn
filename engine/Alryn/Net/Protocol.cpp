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
} // namespace

void write(ByteWriter& w, const PlayerInput& in) {
    w.write_u32(in.sequence);
    w.write_vec3(in.move);
    w.write_f32(in.yaw);
    w.write_f32(in.pitch);
    const u8 flags = static_cast<u8>((in.jump ? 1 : 0) | (in.dig ? 2 : 0) | (in.add ? 4 : 0) |
                                     (in.fire ? 8 : 0));
    w.write_u8(flags);
    w.write_vec3(in.aim);
    write_appearance(w, in.appearance);
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
    in.aim = r.read_vec3();
    read_appearance(r, in.appearance);
    return r.ok();
}

void write(ByteWriter& w, const Snapshot& s) {
    w.write_u32(s.tick);
    w.write_u16(static_cast<u16>(s.players.size()));
    for (const PlayerState& p : s.players) {
        w.write_u32(p.id);
        w.write_vec3(p.position);
        w.write_f32(p.yaw);
        write_appearance(w, p.appearance);
    }
    w.write_u16(static_cast<u16>(s.projectiles.size()));
    for (const ProjectileState& pr : s.projectiles) {
        w.write_vec3(pr.position);
        w.write_u8(pr.kind);
    }
}

bool read(ByteReader& r, Snapshot& s) {
    s.tick = r.read_u32();
    const u16 count = r.read_u16();
    s.players.clear();
    s.players.reserve(count);
    for (u16 i = 0; i < count && r.ok(); ++i) {
        PlayerState p;
        p.id = r.read_u32();
        p.position = r.read_vec3();
        p.yaw = r.read_f32();
        read_appearance(r, p.appearance);
        s.players.push_back(p);
    }
    const u16 proj_count = r.read_u16();
    s.projectiles.clear();
    s.projectiles.reserve(proj_count);
    for (u16 i = 0; i < proj_count && r.ok(); ++i) {
        ProjectileState pr;
        pr.position = r.read_vec3();
        pr.kind = r.read_u8();
        s.projectiles.push_back(pr);
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
