#include <Alryn/Net/Protocol.h>

namespace alryn::net {

void write(ByteWriter& w, const PlayerInput& in) {
    w.write_u32(in.sequence);
    w.write_vec3(in.move);
    w.write_f32(in.yaw);
    w.write_f32(in.pitch);
    const u8 flags = static_cast<u8>((in.jump ? 1 : 0) | (in.dig ? 2 : 0) | (in.add ? 4 : 0));
    w.write_u8(flags);
    w.write_vec3(in.aim);
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
    in.aim = r.read_vec3();
    return r.ok();
}

void write(ByteWriter& w, const Snapshot& s) {
    w.write_u32(s.tick);
    w.write_u16(static_cast<u16>(s.players.size()));
    for (const PlayerState& p : s.players) {
        w.write_u32(p.id);
        w.write_vec3(p.position);
        w.write_f32(p.yaw);
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
        s.players.push_back(p);
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
