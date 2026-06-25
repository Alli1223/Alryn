// ClientApp - particle pool and ability/buff visual effects.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

#include <Alryn/Terrain/WorldGen.h> // ground height + water level for the roaming deer

namespace alryn::game {

void ClientApp::spawn_primary_vfx() {
    const Vec3 feet = local_feet();
    const Vec3 facing{std::cos(face_yaw_), 0.0f, std::sin(face_yaw_)};
    const Vec3 hand = feet + Vec3{0.0f, 1.1f, 0.0f} + facing * 0.5f;
    Vec3 dir = facing;
    if (aim_valid_) {
        Vec3 d = aim_ - hand;
        if (glm::length(d) > 0.3f) {
            dir = glm::normalize(d);
        }
    }
    if (role_ == PlayerRole::Hunter) {
        emit(hand, Vec3{0.0f}, Vec4{0.85f, 1.0f, 0.7f, 1.0f}, 0.12f, 0.3f, 1); // bow flash
        for (int i = 0; i < 9; ++i) {
            emit(hand, dir * frand(4.0f, 9.0f) + rand_dir() * 1.0f,
                 Vec4{0.7f, 1.0f, 0.65f, 0.9f}, 0.3f, 0.1f, 1);
        }
    } else if (role_ == PlayerRole::Cleric) {
        // Arcane motes gather + burst forward in violet.
        emit(hand, Vec3{0.0f}, Vec4{0.7f, 0.45f, 1.0f, 1.0f}, 0.18f, 0.42f, 1);
        for (int i = 0; i < 16; ++i) {
            const Vec3 v = dir * frand(2.5f, 7.0f) + rand_dir() * 1.6f;
            emit(hand + rand_dir() * 0.25f, v, Vec4{0.78f, 0.5f, 1.0f, 0.95f}, 0.45f, 0.12f, 1);
        }
    }
}

void ClientApp::emit(const Vec3& pos, const Vec3& vel, const Vec4& color, f32 life, f32 size, u8 style, f32 gravity, f32 drag) {
    if (particles_.size() > vfx::max_particles) {
        return; // hard cap so a busy fight can't run away with the pool
    }
    Particle p;
    p.pos = pos;
    p.vel = vel;
    p.color = color;
    p.life = life;
    p.max_life = life;
    p.size = size;
    p.style = style;
    p.gravity = gravity;
    p.drag = drag;
    particles_.push_back(p);
}

void ClientApp::emit_burst(const Vec3& center, const Vec4& color, int n, f32 speed, f32 life, f32 size, u8 style, f32 up, f32 gravity) {
    for (int i = 0; i < n; ++i) {
        Vec3 d = rand_dir();
        d.y = std::abs(d.y) * 0.6f;
        emit(center, d * frand(0.3f, 1.0f) * speed + Vec3{0.0f, up, 0.0f},
             Vec4{Vec3{color}, color.a * frand(0.7f, 1.0f)}, life * frand(0.7f, 1.0f),
             size * frand(0.7f, 1.2f), style, gravity);
    }
}

void ClientApp::emit_ring(const Vec3& center, const Vec4& color, int n, f32 speed, f32 life, f32 size, u8 style) {
    for (int i = 0; i < n; ++i) {
        const f32 a = TwoPi * static_cast<f32>(i) / static_cast<f32>(n) + frand(-0.1f, 0.1f);
        const Vec3 dir{std::cos(a), 0.0f, std::sin(a)};
        emit(center + Vec3{0.0f, 0.15f, 0.0f}, dir * speed + Vec3{0.0f, frand(0.3f, 1.0f), 0.0f},
             color, life, size, style, 0.0f, 2.4f);
    }
}

void ClientApp::update_particles(Timestep dt) {
    const f32 s = dt.seconds;
    for (Particle& p : particles_) {
        p.life -= s;
        p.vel *= std::max(0.0f, 1.0f - p.drag * s);
        p.vel.y -= p.gravity * s;
        p.pos += p.vel * s;
    }
    std::erase_if(particles_, [](const Particle& p) { return p.life <= 0.0f; });

    // Glowing trails behind in-flight holy bolts / arrows.
    if (have_snapshot_) {
        for (const net::ProjectileState& pr : snapshot_.projectiles) {
            if (pr.kind == 2) {
                emit(pr.position, rand_dir() * 0.3f, Vec4{0.55f, 1.0f, 0.8f, 0.9f}, 0.4f, 0.16f, 1);
            } else if (pr.kind == 4) { // arcane bolt: a swirling violet trail
                emit(pr.position, rand_dir() * 0.5f, Vec4{0.72f, 0.45f, 1.0f, 0.95f}, 0.45f, 0.15f, 1);
            } else if (pr.kind == 3) {
                emit(pr.position, Vec3{0.0f}, Vec4{0.85f, 0.95f, 0.7f, 0.5f}, 0.25f, 0.07f, 1);
            }
        }
        // Motes rising out of each ground aura, tinted by its kind (heal = gentle, drifting;
        // consecration = flickering holy-fire that climbs faster).
        for (const net::AuraState& a : snapshot_.auras) {
            const AuraProps props = aura_props(static_cast<AuraKind>(a.kind));
            const bool fire = static_cast<AuraKind>(a.kind) == AuraKind::Consecration;
            for (int i = 0; i < (fire ? 3 : 2); ++i) {
                const f32 ang = frand(0.0f, TwoPi);
                const f32 rr = frand(0.0f, a.radius);
                const Vec3 p = a.position + Vec3{std::cos(ang) * rr, 0.05f, std::sin(ang) * rr};
                const f32 rise = fire ? frand(1.4f, 3.0f) : frand(0.9f, 2.0f);
                emit(p, Vec3{0.0f, rise, 0.0f}, Vec4{props.color, 0.88f}, fire ? 0.5f : 0.8f,
                     fire ? 0.13f : 0.1f, 1, fire ? -1.4f : -0.8f);
            }
        }
        // Shimmer sparkles orbiting each Aegis shield bubble.
        auto sparkle = [&](const Vec3& feet, f32 strength) {
            if (strength > 0.02f) {
                const Vec3 d = rand_dir();
                emit(feet + Vec3{0.0f, 1.0f, 0.0f} + d * 1.15f, d * 0.2f,
                     Vec4{0.6f, 0.85f, 1.0f, 0.8f * strength}, 0.4f, 0.08f, 1);
            }
        };
        for (const net::PlayerState& p : snapshot_.players) {
            sparkle(p.position, static_cast<f32>(p.shield) / 255.0f);
        }
        for (const net::VillagerState& v : snapshot_.villagers) {
            sparkle(v.position, static_cast<f32>(v.shield) / 255.0f);
        }
    }
}

void ClientApp::draw_auras() {
    if (renderer_ == nullptr || !have_snapshot_) {
        return;
    }
    const f32 night = 1.0f - sun_intensity_;
    for (const net::AuraState& a : snapshot_.auras) {
        const AuraProps props = aura_props(static_cast<AuraKind>(a.kind));
        const f32 r = a.radius;
        renderer_->draw_glow(shape_sphere_,
                             glm::translate(Mat4{1.0f}, a.position + Vec3{0.0f, 0.08f, 0.0f}) *
                                 glm::scale(Mat4{1.0f}, Vec3{r, 0.1f, r}),
                             Vec4{props.color, 0.32f}); // ground disc
        renderer_->draw_glow(shape_sphere_,
                             glm::translate(Mat4{1.0f}, a.position + Vec3{0.0f, 0.3f, 0.0f}) *
                                 glm::scale(Mat4{1.0f}, Vec3{r * 0.95f, r * 0.5f, r * 0.95f}),
                             Vec4{props.color, 0.1f}); // soft dome
        // A gentle (unshadowed) light so the aura illuminates the ground at night.
        if (props.light > 0.0f && night > 0.12f) {
            Renderer::SpotLight sl;
            sl.position = a.position + Vec3{0.0f, 2.0f, 0.0f};
            sl.direction = Vec3{0.0f, -1.0f, 0.0f};
            sl.color = props.color * (props.light * night);
            sl.range = r * 2.4f;
            sl.cone_outer_cos = std::cos(glm::radians(75.0f));
            sl.cone_inner_cos = std::cos(glm::radians(45.0f));
            sl.cast_shadow = false;
            renderer_->add_light(sl);
        }
    }
}

void ClientApp::draw_shields() {
    if (renderer_ == nullptr || !have_snapshot_) {
        return;
    }
    const f32 pulse = 1.0f + 0.04f * std::sin(elapsed_ * 6.0f);
    auto bubble = [&](const Vec3& feet, f32 strength) {
        if (strength <= 0.02f) {
            return;
        }
        const Vec3 c = feet + Vec3{0.0f, 1.0f, 0.0f};
        const f32 rad = 1.15f * pulse;
        renderer_->draw_glow(shape_sphere_,
                             glm::translate(Mat4{1.0f}, c) * glm::scale(Mat4{1.0f}, Vec3{rad}),
                             Vec4{0.45f, 0.7f, 1.0f, 0.16f * strength});
        renderer_->draw_transparent(shape_sphere_,
                                    glm::translate(Mat4{1.0f}, c) *
                                        glm::scale(Mat4{1.0f}, Vec3{rad * 1.02f}),
                                    Vec4{0.55f, 0.8f, 1.0f, 0.13f * strength});
    };
    for (const net::PlayerState& p : snapshot_.players) {
        bubble(p.position, static_cast<f32>(p.shield) / 255.0f);
    }
    for (const net::VillagerState& v : snapshot_.villagers) {
        bubble(v.position, static_cast<f32>(v.shield) / 255.0f);
    }
}

void ClientApp::draw_particles() {
    if (renderer_ == nullptr) {
        return;
    }
    for (const Particle& p : particles_) {
        const f32 t = glm::clamp(p.life / p.max_life, 0.0f, 1.0f);
        const f32 sz = p.size * (0.35f + 0.65f * t);
        const Mat4 m = glm::translate(Mat4{1.0f}, p.pos) * glm::scale(Mat4{1.0f}, Vec3{sz});
        const Vec4 col{Vec3{p.color}, p.color.a * t};
        if (p.style == 1) {
            renderer_->draw_glow(shape_sphere_, m, col);
        } else {
            renderer_->draw_emissive(shape_sphere_, m, col);
        }
    }
}

void ClientApp::draw_ambient_life() {
    if (renderer_ == nullptr) {
        return;
    }
    const Vec3 feet = local_feet();
    const f32 t = elapsed_;
    const f32 night = 1.0f - sun_intensity_;
    // 0..1 hash of a world cell + salt (deterministic per spot, so it stays put as you move).
    auto hcell = [](int x, int z, int s) {
        u32 v = static_cast<u32>(x * 73856093) ^ static_cast<u32>(z * 19349663) ^
                static_cast<u32>(s * 83492791);
        v ^= v >> 13;
        v *= 0x2545F491u;
        v ^= v >> 16;
        return static_cast<f32>((v >> 8) & 0xFFFFu) / 65535.0f;
    };

    // Fireflies: blinking glow motes anchored to fixed WORLD cells around the player. Each lives at
    // a fixed world spot (so walking moves you THROUGH the swarm instead of dragging it along) and
    // only drifts gently about that spot. They settle just above the real ground and fade in at the
    // view edge so there's no pop-in as cells enter/leave range.
    if (night > 0.2f) {
        const f32 cs = 4.5f;   // cell size (firefly spacing)
        const int cr = 4;      // cell radius around the player (~18 m)
        const int bcx = static_cast<int>(std::floor(feet.x / cs));
        const int bcz = static_cast<int>(std::floor(feet.z / cs));
        for (int dz = -cr; dz <= cr; ++dz) {
            for (int dx = -cr; dx <= cr; ++dx) {
                const int cx = bcx + dx, cz = bcz + dz;
                if (hcell(cx, cz, 3) > 0.45f) {
                    continue; // only ~45% of cells host a firefly
                }
                const f32 ax = (static_cast<f32>(cx) + hcell(cx, cz, 5)) * cs;
                const f32 az = (static_cast<f32>(cz) + hcell(cx, cz, 7)) * cs;
                const f32 g = worldgen::height(ax, az, world_seed_);
                if (g < worldgen::water_level + 0.4f) {
                    continue; // no fireflies out over the water
                }
                const f32 ph = hcell(cx, cz, 9) * TwoPi;
                const f32 px = ax + std::sin(t * 0.5f + ph) * 1.1f + std::sin(t * 0.21f + ph * 1.7f) * 0.5f;
                const f32 pz = az + std::cos(t * 0.43f + ph) * 1.1f;
                const f32 py = g + 0.7f + 0.5f * std::sin(t * 0.9f + ph * 2.0f);
                const f32 dxz = glm::length(Vec2{px - feet.x, pz - feet.z});
                if (dxz > 18.0f) {
                    continue;
                }
                const f32 blink = std::pow(0.5f + 0.5f * std::sin(t * 2.3f + ph * 3.1f), 3.0f);
                const f32 edge = glm::smoothstep(18.0f, 12.0f, dxz); // soft fade at the radius
                const f32 a = night * (0.18f + 0.82f * blink) * 0.6f * edge;
                renderer_->draw_glow(
                    shape_sphere_,
                    glm::translate(Mat4{1.0f}, Vec3{px, py, pz}) * glm::scale(Mat4{1.0f}, Vec3{0.07f}),
                    Vec4{0.78f, 0.96f, 0.42f, a});
            }
        }
    }

    // Daytime DUST / POLLEN motes drifting in the light - warm pale specks anchored to fixed world
    // cells (like the fireflies), so you move THROUGH them rather than dragging them along. They
    // swirl gently + bob up and down, twinkle as they catch the light, and fade out at the view edge.
    if (night < 0.6f) {
        const f32 day = 1.0f - night; // stronger in full daylight, gone by dusk
        const f32 cs = 3.2f;          // cell size (mote spacing)
        const int cr = 4;             // cells around the player
        const int bcx = static_cast<int>(std::floor(feet.x / cs));
        const int bcz = static_cast<int>(std::floor(feet.z / cs));
        for (int dz = -cr; dz <= cr; ++dz) {
            for (int dx = -cr; dx <= cr; ++dx) {
                const int cx = bcx + dx, cz = bcz + dz;
                if (hcell(cx, cz, 13) > 0.62f) {
                    continue; // ~62% of cells host a mote
                }
                const f32 ax = (static_cast<f32>(cx) + hcell(cx, cz, 15)) * cs;
                const f32 az = (static_cast<f32>(cz) + hcell(cx, cz, 17)) * cs;
                const f32 g = worldgen::height(ax, az, world_seed_);
                if (g < worldgen::water_level + 0.3f) {
                    continue; // not out over the water
                }
                const f32 ph = hcell(cx, cz, 19) * TwoPi;
                const f32 px = ax + std::sin(t * 0.32f + ph) * 1.3f + std::sin(t * 0.13f + ph * 2.1f) * 0.7f;
                const f32 pz = az + std::cos(t * 0.27f + ph) * 1.3f;
                const f32 py = g + 0.9f + 1.1f * (0.5f + 0.5f * std::sin(t * 0.4f + ph * 1.6f));
                const f32 dxz = glm::length(Vec2{px - feet.x, pz - feet.z});
                if (dxz > 16.0f) {
                    continue;
                }
                const f32 edge = glm::smoothstep(16.0f, 10.0f, dxz);
                const f32 twinkle =
                    0.35f + 0.65f * std::pow(0.5f + 0.5f * std::sin(t * 1.3f + ph * 4.0f), 2.0f);
                const f32 a = day * 0.7f * twinkle * edge;
                renderer_->draw_glow(shape_sphere_,
                                     glm::translate(Mat4{1.0f}, Vec3{px, py, pz}) *
                                         glm::scale(Mat4{1.0f}, Vec3{0.06f}),
                                     Vec4{1.0f, 0.95f, 0.74f, a}); // warm sun-catching pollen/dust
            }
        }
    }
    // Falling autumn LEAVES drifting down in the woods - tumbling, autumn-tinted, anchored to world
    // cells (so you walk through the fall instead of dragging it along) and gated to FOREST biomes so
    // they never appear over desert / snow / open water. Each cell sheds one leaf that falls + sways +
    // tumbles, fading in at the top + out near the ground so the loop doesn't pop.
    {
        const f32 cs = 6.0f; // sparse - a gentle drift, not a blizzard
        const int cr = 3;    // ~18 m of cells around the player
        const int bcx = static_cast<int>(std::floor(feet.x / cs));
        const int bcz = static_cast<int>(std::floor(feet.z / cs));
        const Vec3 autumn[4] = {{0.82f, 0.58f, 0.18f},  // gold
                                {0.86f, 0.42f, 0.14f},  // orange
                                {0.70f, 0.26f, 0.15f},  // russet red
                                {0.55f, 0.40f, 0.20f}}; // brown
        for (int dz = -cr; dz <= cr; ++dz) {
            for (int dx = -cr; dx <= cr; ++dx) {
                const int cx = bcx + dx, cz = bcz + dz;
                if (hcell(cx, cz, 21) > 0.55f) {
                    continue; // ~55% of forest cells shed a leaf
                }
                const f32 ax = (static_cast<f32>(cx) + hcell(cx, cz, 23)) * cs;
                const f32 az = (static_cast<f32>(cz) + hcell(cx, cz, 25)) * cs;
                if (worldgen::biome_at(ax, az, world_seed_) != worldgen::Biome::Forest) {
                    continue; // only in the woods
                }
                const f32 g = worldgen::height(ax, az, world_seed_);
                if (g < worldgen::water_level + 0.4f) {
                    continue;
                }
                const f32 ph = hcell(cx, cz, 27) * TwoPi;
                const f32 fall = std::fmod(t * 0.16f + ph, 1.0f);          // 0 top .. 1 ground
                const f32 px = ax + std::sin(t * 1.3f + ph * 3.0f) * 0.9f; // sway as it falls
                const f32 pz = az + std::cos(t * 1.0f + ph * 2.0f) * 0.9f;
                const f32 py = g + 0.2f + 4.4f * (1.0f - fall);
                const f32 dxz = glm::length(Vec2{px - feet.x, pz - feet.z});
                if (dxz > 17.0f) {
                    continue;
                }
                const f32 edge = glm::smoothstep(17.0f, 11.0f, dxz);
                const f32 life = glm::smoothstep(0.0f, 0.08f, fall) * glm::smoothstep(1.0f, 0.88f, fall);
                const Vec3 col = autumn[static_cast<int>(hcell(cx, cz, 29) * 4.0f) & 3];
                const Mat4 m = glm::translate(Mat4{1.0f}, Vec3{px, py, pz}) *
                               glm::rotate(Mat4{1.0f}, t * 1.8f + ph, Vec3{0.0f, 1.0f, 0.0f}) *
                               glm::rotate(Mat4{1.0f}, std::sin(t * 2.2f + ph) * 0.9f, Vec3{0.0f, 0.0f, 1.0f}) *
                               glm::scale(Mat4{1.0f}, Vec3{0.18f, 0.04f, 0.22f}); // a small flat leaf
                renderer_->draw_transparent(shape_sphere_, m, Vec4{col, 0.9f * edge * life});
            }
        }
    }
    // (The day "bird flock" + night owl were removed - they orbited the camera at a fixed offset,
    // so they read as stationary shapes floating behind the player with shadows that didn't move.)
}

void ClientApp::update_deer(Timestep dt) {
    if (renderer_ == nullptr) {
        return;
    }
    const Vec3 feet = local_feet();
    auto ground = [&](f32 x, f32 z) { return worldgen::height(x, z, world_seed_); };
    // Maintain a small roaming herd around the player.
    while (deer_.size() < 6) {
        Deer d;
        const f32 a = frand(0.0f, TwoPi), r = frand(24.0f, 40.0f);
        d.pos = Vec3{feet.x + std::cos(a) * r, 0.0f, feet.z + std::sin(a) * r};
        d.pos.y = ground(d.pos.x, d.pos.z);
        d.yaw = frand(0.0f, TwoPi);
        d.target = d.pos;
        deer_.push_back(d);
    }
    for (Deer& d : deer_) {
        const f32 dist = glm::length(Vec2{d.pos.x - feet.x, d.pos.z - feet.z});
        if (dist > 58.0f || d.pos.y < worldgen::water_level + 0.3f) {
            const f32 a = frand(0.0f, TwoPi), r = frand(26.0f, 40.0f); // respawn around the player
            d.pos = Vec3{feet.x + std::cos(a) * r, 0.0f, feet.z + std::sin(a) * r};
            d.pos.y = ground(d.pos.x, d.pos.z);
            d.retarget = 0.0f;
            d.fleeing = false;
            continue;
        }
        d.fleeing = dist < 11.0f; // bolt if the player gets close
        d.retarget -= dt.seconds;
        if (d.fleeing) {
            const Vec2 away = glm::normalize(Vec2{d.pos.x - feet.x, d.pos.z - feet.z});
            d.target = d.pos + Vec3{away.x, 0.0f, away.y} * 14.0f;
        } else if (d.retarget <= 0.0f) {
            const f32 a = frand(0.0f, TwoPi), r = frand(2.5f, 9.0f);
            d.target = d.pos + Vec3{std::cos(a) * r, 0.0f, std::sin(a) * r};
            d.retarget = frand(2.5f, 6.0f); // graze a while between strolls
        }
        const Vec2 to{d.target.x - d.pos.x, d.target.z - d.pos.z};
        const f32 td = glm::length(to);
        const f32 spd = (d.fleeing ? 8.0f : 1.4f) * dt.seconds;
        if (td > 0.15f) {
            const Vec2 step = to / td * std::min(spd, td);
            d.pos.x += step.x;
            d.pos.z += step.y;
            d.pos.y = ground(d.pos.x, d.pos.z);
            d.yaw = std::atan2(step.y, step.x);
            d.gait += glm::length(step) * 3.2f;
        }
    }
}

void ClientApp::draw_deer() {
    if (renderer_ == nullptr) {
        return;
    }
    for (const Deer& d : deer_) {
        const Mat4 base = glm::translate(Mat4{1.0f}, d.pos) * glm::rotate(Mat4{1.0f}, -d.yaw, Vec3{0.0f, 1.0f, 0.0f});
        renderer_->draw(deer_body_mesh_, base);
        for (int k = 0; k < 4; ++k) {
            const f32 sign = (k == 0 || k == 3) ? 1.0f : -1.0f;
            const f32 swing = std::sin(d.gait) * (d.fleeing ? 0.7f : 0.35f) * sign;
            const Mat4 lm = base * glm::translate(Mat4{1.0f}, kDeerLegs[k]) *
                            glm::rotate(Mat4{1.0f}, swing, Vec3{0.0f, 0.0f, 1.0f});
            renderer_->draw(deer_leg_mesh_, lm);
        }
    }
}

void ClientApp::spawn_ability_vfx(PlayerRole role, u8 slot, const Vec3& feet, f32 yaw, const Vec3& aim) {
    const Vec3 chest = feet + Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 facing{std::cos(yaw), 0.0f, std::sin(yaw)};
    Vec3 to_aim = aim - chest;
    to_aim.y = 0.0f;
    const Vec3 fwd = glm::length(to_aim) > 0.3f ? glm::normalize(to_aim) : facing;
    switch (role) {
        case PlayerRole::Knight:
            if (slot == 0) { // Shield Bash: a steel shockwave punched forward
                const Vec3 c = chest + fwd * 1.2f;
                emit(c, Vec3{0.0f}, Vec4{0.9f, 0.95f, 1.0f, 1.0f}, 0.18f, 0.7f, 1);
                for (int i = 0; i < 26; ++i) {
                    const f32 a = yaw + frand(-0.7f, 0.7f);
                    const Vec3 d{std::cos(a), frand(0.0f, 0.5f), std::sin(a)};
                    emit(c, d * frand(6.0f, 12.0f), Vec4{0.8f, 0.88f, 1.0f, 0.95f}, 0.4f, 0.16f, 1,
                         6.0f);
                }
            } else if (slot == 1 || slot == 5) { // Bulwark / Rally: a golden dome flares up
                emit_ring(feet, Vec4{1.0f, 0.85f, 0.4f, 0.95f}, 22, 3.0f, 0.6f, 0.16f);
                emit_burst(chest, Vec4{1.0f, 0.82f, 0.35f, 0.9f}, 14, 2.5f, 0.7f, 0.14f, 1, 1.5f);
            } else if (slot == 2) { // Consecration: a holy-fire ring erupts from the ground
                emit_ring(feet, Vec4{1.0f, 0.72f, 0.28f, 0.95f}, 30, kConsecrationRadius * 1.6f,
                          0.6f, 0.2f);
                emit_burst(feet + Vec3{0.0f, 0.2f, 0.0f}, Vec4{1.0f, 0.6f, 0.2f, 0.9f}, 22, 2.5f,
                           0.7f, 0.16f, 1, 3.0f);
            } else if (slot == 4) { // Whirlwind: a steel ring sweeps around the knight
                emit_ring(feet + Vec3{0.0f, 0.6f, 0.0f}, Vec4{0.85f, 0.9f, 1.0f, 0.95f}, 30,
                          kWhirlwindRadius * 1.4f, 0.45f, 0.16f);
                emit_burst(chest, Vec4{0.8f, 0.88f, 1.0f, 0.9f}, 16, 5.0f, 0.4f, 0.13f, 1, 1.0f);
            } else { // Taunt: a red warcry pulse + upward embers
                emit_ring(feet, Vec4{1.0f, 0.3f, 0.25f, 0.95f}, 24, 7.0f, 0.5f, 0.18f);
                emit_burst(chest, Vec4{1.0f, 0.4f, 0.3f, 0.9f}, 14, 3.0f, 0.55f, 0.15f, 1, 3.0f);
            }
            break;
        case PlayerRole::Hunter:
            if (slot == 2) { // Dash: a green backward kick of speed-motes
                emit_burst(feet + Vec3{0.0f, 0.4f, 0.0f}, Vec4{0.6f, 1.0f, 0.6f, 0.9f}, 18,
                           -4.0f, 0.5f, 0.13f, 1);
                for (int i = 0; i < 12; ++i) {
                    emit(chest - facing * frand(0.0f, 0.8f), -facing * frand(2.0f, 5.0f),
                         Vec4{0.7f, 1.0f, 0.7f, 0.8f}, 0.4f, 0.12f, 1);
                }
            } else if (slot == 5) { // Caltrops: a low scatter ring on the ground at the aim
                emit_ring(aim, Vec4{0.9f, 0.82f, 0.3f, 0.9f}, 20, kHazardRadius * 1.3f, 0.5f, 0.13f);
                emit_burst(aim + Vec3{0.0f, 0.15f, 0.0f}, Vec4{0.85f, 0.78f, 0.3f, 0.85f}, 12, 2.0f,
                           0.5f, 0.11f, 1, 1.2f);
            } else { // Power Shot / Volley / Multishot: a bright muzzle spray along the shot
                const Vec3 c = chest + fwd * 0.8f;
                emit(c, Vec3{0.0f}, Vec4{0.8f, 1.0f, 0.7f, 1.0f}, 0.14f, 0.4f, 1);
                const bool wide = slot == 1 || slot == 4; // Volley / Multishot fan wider
                const f32 spread = wide ? 0.4f : 0.15f;
                for (int i = 0; i < (wide ? 22 : 12); ++i) {
                    const f32 a = std::atan2(fwd.z, fwd.x) + frand(-spread, spread);
                    emit(c, Vec3{std::cos(a), frand(-0.1f, 0.2f), std::sin(a)} * frand(5.0f, 11.0f),
                         Vec4{0.7f, 1.0f, 0.65f, 0.9f}, 0.35f, 0.12f, 1);
                }
            }
            break;
        case PlayerRole::Cleric:
            if (slot == 1) { // Sanctuary: a wide radiant ring + a column of light
                emit_ring(feet, Vec4{0.6f, 1.0f, 0.8f, 0.95f}, 30, 6.0f, 0.8f, 0.2f);
                for (int i = 0; i < 24; ++i) {
                    emit(feet + Vec3{frand(-1.2f, 1.2f), frand(0.0f, 0.3f), frand(-1.2f, 1.2f)},
                         Vec3{0.0f, frand(2.0f, 4.5f), 0.0f}, Vec4{0.7f, 1.0f, 0.85f, 0.9f}, 0.9f,
                         0.14f, 1, -1.0f);
                }
            } else if (slot == 2 || slot == 5) { // Smite / Judgement: a holy flash punched forward
                const Vec3 c = chest + fwd * 0.8f;
                emit(c, Vec3{0.0f}, Vec4{0.85f, 1.0f, 0.9f, 1.0f}, 0.2f, 0.6f, 1);
                emit_burst(c, Vec4{0.7f, 1.0f, 0.85f, 0.95f}, 16, 6.0f, 0.4f, 0.14f, 1);
            } else if (slot == 3) { // Aegis: a cyan ward flares at the caster (sphere is on the target)
                emit(chest, Vec3{0.0f}, Vec4{0.55f, 0.85f, 1.0f, 1.0f}, 0.2f, 0.5f, 1);
                emit_ring(feet, Vec4{0.5f, 0.8f, 1.0f, 0.9f}, 18, 3.0f, 0.5f, 0.14f);
                emit_burst(chest, Vec4{0.6f, 0.9f, 1.0f, 0.9f}, 14, 2.5f, 0.6f, 0.13f, 1, 1.5f);
            } else { // Heal: gentle motes rising around the caster
                for (int i = 0; i < 18; ++i) {
                    emit(feet + Vec3{frand(-0.5f, 0.5f), frand(0.1f, 0.4f), frand(-0.5f, 0.5f)},
                         Vec3{frand(-0.3f, 0.3f), frand(1.6f, 3.2f), frand(-0.3f, 0.3f)},
                         Vec4{0.55f, 1.0f, 0.7f, 0.9f}, 0.8f, 0.12f, 1, -1.2f);
                }
                emit_ring(feet, Vec4{0.55f, 1.0f, 0.7f, 0.8f}, 14, 2.0f, 0.5f, 0.13f);
            }
            break;
        case PlayerRole::Mage:
            break; // Mage spells have their own VFX (spawn_spell_vfx); the hotbar only queues elements
    }
}

void ClientApp::update_ability_vfx(Timestep dt) {
    const Vec3 feet = local_feet();
    if (bulwark_fx_ > 0.0f) { // a golden shield dome orbiting the local Knight
        bulwark_fx_ -= dt.seconds;
        const f32 a = elapsed_ * 5.0f;
        for (int i = 0; i < 3; ++i) {
            const f32 ang = a + TwoPi * static_cast<f32>(i) / 3.0f;
            emit(feet + Vec3{std::cos(ang) * 0.9f, 0.9f + 0.4f * std::sin(a * 0.7f), std::sin(ang) * 0.9f},
                 Vec3{0.0f}, Vec4{1.0f, 0.82f, 0.35f, 0.7f}, 0.25f, 0.13f, 1);
        }
    }
    if (dash_fx_ > 0.0f) { // a green speed-trail behind the local Hunter
        dash_fx_ -= dt.seconds;
        emit(feet + Vec3{0.0f, 0.5f, 0.0f}, Vec3{0.0f}, Vec4{0.6f, 1.0f, 0.6f, 0.6f}, 0.3f, 0.12f, 1);
    }
    // Cleric heal channel (right mouse held): mirror the server's charge for a charge bar +
    // gathering VFX; a burst of green motes converges on the staff, and on a full charge it
    // releases (resets) - the actual aura is server-spawned + networked.
    if (role_ == PlayerRole::Cleric && blocking_) {
        heal_charge_fx_ += dt.seconds;
        const Vec3 head = feet + Vec3{0.0f, 1.5f, 0.0f};
        for (int i = 0; i < 3; ++i) {
            const Vec3 from = head + rand_dir() * frand(1.0f, 2.2f);
            emit(from, (head - from) * frand(2.0f, 4.0f), Vec4{0.5f, 1.0f, 0.7f, 0.9f}, 0.4f, 0.1f, 1);
        }
        if (heal_charge_fx_ >= kHealChargeTime) {
            emit_ring(feet, Vec4{0.5f, 1.0f, 0.7f, 0.95f}, 28, 5.0f, 0.7f, 0.18f);
            emit_burst(feet + Vec3{0.0f, 0.3f, 0.0f}, Vec4{0.6f, 1.0f, 0.8f, 0.9f}, 20, 3.0f, 0.8f, 0.14f, 1, 2.0f);
            heal_charge_fx_ = 0.0f;
        }
    } else {
        heal_charge_fx_ = 0.0f;
    }
    if (!have_snapshot_) {
        return;
    }
    for (const net::PlayerState& p : snapshot_.players) {
        if (p.cast == 0 || p.id == my_id_) {
            continue; // local casts are played instantly on keypress
        }
        if (ability_fx_tick_[p.id] == snapshot_.tick) {
            continue; // already played for this snapshot
        }
        ability_fx_tick_[p.id] = snapshot_.tick;
        const Vec3 facing{std::cos(p.yaw), 0.0f, std::sin(p.yaw)};
        spawn_ability_vfx(static_cast<PlayerRole>(p.role % kRoleCount),
                          static_cast<u8>(p.cast - 1), p.position, p.yaw, p.position + facing);
    }
}

f32 ClientApp::frand() { // xorshift32 -> [0,1)
    fx_rng_ ^= fx_rng_ << 13;
    fx_rng_ ^= fx_rng_ >> 17;
    fx_rng_ ^= fx_rng_ << 5;
    return static_cast<f32>(fx_rng_ & 0xffffffu) / static_cast<f32>(0x1000000u);
}

Vec3 ClientApp::rand_dir() {
    const f32 z = frand(-1.0f, 1.0f);
    const f32 a = frand(0.0f, TwoPi);
    const f32 r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return Vec3{r * std::cos(a), z, r * std::sin(a)};
}

void ClientApp::draw_buffs() {
    if (!have_snapshot_ || renderer_ == nullptr) {
        return;
    }
    const net::WagonState* aw = active_wagon();
    const f32 pulse = 0.65f + 0.35f * std::sin(elapsed_ * 6.0f);
    for (const net::PlayerState& p : snapshot_.players) {
        if (p.buffs == 0) {
            continue;
        }
        const Vec3 feet = (p.seated != 0 && aw != nullptr) ? attach_to_wagon(*aw, p.position)
                                                           : p.position;
        if ((p.buffs & 1u) != 0u) { // empowered: a fiery ring + rising embers
            renderer_->draw_glow(shape_cylinder_,
                                 glm::translate(Mat4{1.0f}, feet + Vec3{0.0f, 0.06f, 0.0f}) *
                                     glm::scale(Mat4{1.0f}, Vec3{1.15f, 0.05f, 1.15f}),
                                 Vec4{1.0f, 0.45f, 0.15f, 0.5f * pulse});
        }
        if ((p.buffs & 2u) != 0u) { // hasted: a green ring
            renderer_->draw_glow(shape_cylinder_,
                                 glm::translate(Mat4{1.0f}, feet + Vec3{0.0f, 0.13f, 0.0f}) *
                                     glm::scale(Mat4{1.0f}, Vec3{0.92f, 0.05f, 0.92f}),
                                 Vec4{0.4f, 1.0f, 0.45f, 0.5f * pulse});
        }
    }
}

void ClientApp::draw_rain() {
    if (renderer_ == nullptr) {
        return;
    }
    // Rain only sets in once the sky is well past overcast.
    const f32 rain = glm::smoothstep(0.28f, 0.72f, weather_amt_);
    if (rain <= 0.01f) {
        return;
    }
    const Vec3 feet = local_feet();
    const f32 t = elapsed_;

    // World-anchored falling streaks in a column around the player. Each world cell may hold one
    // drop whose xz is FIXED in world space, so moving the camera gives real parallax (the drops
    // sit in the world, not on the glass). Every drop falls at a CONSTANT speed - storminess only
    // changes how many cells spawn a drop and the opacity - so when the rain eases off it simply
    // thins out instead of running backwards up the screen.
    auto hcell = [](int x, int z, int s) {
        u32 v = static_cast<u32>(x * 73856093) ^ static_cast<u32>(z * 19349663) ^
                static_cast<u32>(s * 83492791);
        v ^= v >> 13;
        v *= 0x2545F491u;
        v ^= v >> 16;
        return static_cast<f32>((v >> 8) & 0xFFFFu) / 65535.0f;
    };

    const f32 cs = 1.3f;        // drop spacing (cell size)
    const int cr = 9;           // cell radius around the player (~12 m column)
    const f32 ceil_h = 13.0f;   // spawn height above the player's feet
    const f32 fall_h = 17.0f;   // distance a drop falls before it recycles to the top
    const f32 speed = 22.0f;    // constant fall speed (m/s) - the key to no reversal
    const f32 drift = 0.5f;     // how much the wind carries a drop sideways as it falls

    // A slowly turning horizontal breeze, stronger in a heavier storm.
    const f32 wdir = t * 0.05f;
    const Vec2 wind = Vec2{std::cos(wdir), std::sin(wdir)} * (1.5f + 4.5f * weather_amt_);
    // The streak points along the drop's instantaneous velocity (fall + wind carry).
    const Mat4 orient =
        orient_to(glm::normalize(Vec3{wind.x * drift, -speed, wind.y * drift}));
    const Vec4 col{0.74f, 0.81f, 0.94f, 0.18f + 0.30f * rain};

    const int pcx = static_cast<int>(std::floor(feet.x / cs));
    const int pcz = static_cast<int>(std::floor(feet.z / cs));
    for (int dz = -cr; dz <= cr; ++dz) {
        for (int dx = -cr; dx <= cr; ++dx) {
            const int cx = pcx + dx, cz = pcz + dz;
            if (hcell(cx, cz, 7) > 0.20f + 0.80f * rain) {
                continue; // sparse in light rain, full coverage in a downpour
            }
            const f32 base_x = (static_cast<f32>(cx) + hcell(cx, cz, 11)) * cs;
            const f32 base_z = (static_cast<f32>(cz) + hcell(cx, cz, 13)) * cs;
            // Per-cell phase offset so the column doesn't fall in lockstep.
            const f32 fallen = std::fmod(t * speed + hcell(cx, cz, 17) * fall_h, fall_h);
            const f32 wy = feet.y + ceil_h - fallen;
            if (wy < feet.y - 3.0f) {
                continue; // it's reached the ground - cull until it recycles
            }
            const f32 age = fallen / speed; // seconds since this drop spawned at the top
            const Vec3 pos{base_x + wind.x * drift * age, wy, base_z + wind.y * drift * age};
            const f32 len = 0.5f + 0.4f * hcell(cx, cz, 23);
            renderer_->draw_transparent(shape_box_,
                                        glm::translate(Mat4{1.0f}, pos) * orient *
                                            glm::scale(Mat4{1.0f}, Vec3{0.018f, 0.018f, len}),
                                        col);
        }
    }
}

void ClientApp::draw_weather() {
    if (renderer_ == nullptr || lightning_ < 0.01f) {
        return;
    }
    // The only genuinely screen-space part: a brief full-screen bluish-white lightning wash
    // (timed in update_day_night). The rain itself is world-space - see draw_rain().
    const VkExtent2D ext = renderer_->extent();
    const f32 W = static_cast<f32>(ext.width);
    const f32 H = static_cast<f32>(ext.height);
    ui::DrawList draw{*renderer_};
    draw.rect(Vec4{0.0f, 0.0f, W, H},
              Vec4{0.88f, 0.92f, 1.0f, lightning_ * 0.45f * std::max(weather_amt_, 0.5f)});
}

} // namespace alryn::game
