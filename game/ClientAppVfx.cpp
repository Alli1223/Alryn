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
        for (int i = 0; i < 12; ++i) {
            emit(hand, dir * frand(4.0f, 9.0f) + rand_dir() * 1.0f,
                 Vec4{0.7f, 1.0f, 0.65f, 0.9f}, 0.3f, 0.1f, 1);
        }
        flash_light(hand, Vec3{0.7f, 1.0f, 0.6f}, 1.2f, 5.0f, 0.15f);
    } else if (role_ == PlayerRole::Cleric) {
        // Arcane motes gather + burst forward in violet.
        emit(hand, Vec3{0.0f}, Vec4{0.7f, 0.45f, 1.0f, 1.0f}, 0.18f, 0.42f, 1);
        for (int i = 0; i < 20; ++i) {
            const Vec3 v = dir * frand(2.5f, 7.0f) + rand_dir() * 1.6f;
            emit(hand + rand_dir() * 0.25f, v, Vec4{0.78f, 0.5f, 1.0f, 0.95f}, 0.45f, 0.12f, 1);
        }
        flash_light(hand, Vec3{0.65f, 0.45f, 1.0f}, 2.0f, 6.0f, 0.25f);
    }
}

bool ClientApp::emit(const Vec3& pos, const Vec3& vel, const Vec4& color, f32 life, f32 size, u8 style, f32 gravity, f32 drag) {
    if (particles_.size() > vfx::max_particles) {
        return false; // hard cap so a busy fight can't run away with the pool
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
    return true;
}

void ClientApp::emit_ember(const Vec3& pos, const Vec3& vel, const Vec3& hot, const Vec3& cool, f32 life,
                           f32 size, f32 gravity, f32 alpha) {
    if (emit(pos, vel, Vec4{hot, alpha}, life, size, 1, gravity)) {
        particles_.back().tail = cool;
    }
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

void ClientApp::emit_splash(const Vec3& at, f32 intensity) {
    const int n = 4 + static_cast<int>(glm::clamp(intensity, 0.0f, 7.0f));
    // Droplets spray up + out and arc back down under gravity (white-blue, lightly transparent).
    emit_burst(at + Vec3{0.0f, 0.05f, 0.0f}, Vec4{0.86f, 0.94f, 1.0f, 0.9f}, n,
               1.6f + intensity * 0.25f, 0.45f, 0.06f, /*style=*/1, /*up=*/2.0f + intensity * 0.2f,
               /*gravity=*/7.5f);
    // A low ripple ring skating outward across the surface.
    emit_ring(at, Vec4{0.92f, 0.97f, 1.0f, 0.5f}, 7, 1.2f, 0.5f, 0.05f);
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

    // Beams, sigils and light flashes age out; meteors fall (shedding embers) and burst on arrival.
    for (Beam& b : beams_) {
        b.life -= s;
        b.age += s;
    }
    std::erase_if(beams_, [](const Beam& b) { return b.life <= 0.0f; });
    for (Glyph& g : glyphs_) {
        g.life -= s;
    }
    std::erase_if(glyphs_, [](const Glyph& g) { return g.life <= 0.0f; });
    for (FlashLight& f : flash_lights_) {
        f.life -= s;
    }
    std::erase_if(flash_lights_, [](const FlashLight& f) { return f.life <= 0.0f; });
    const Vec3 ember_hot{1.0f, 0.86f, 0.45f};
    const Vec3 ember_cool{0.75f, 0.12f, 0.04f};
    for (usize i = 0; i < meteors_.size(); ++i) {
        MeteorFx& m = meteors_[i];
        m.t += s;
        const f32 k = glm::clamp(m.t / m.dur, 0.0f, 1.0f);
        const Vec3 p = glm::mix(m.from, m.to, k * k);
        const Vec3 back = glm::normalize(m.from - m.to);
        for (int e = 0; e < 4; ++e) {
            emit_ember(p + rand_dir() * 0.35f, back * frand(2.0f, 5.0f) + rand_dir() * 1.2f, ember_hot,
                       ember_cool, frand(0.35f, 0.6f), frand(0.25f, 0.45f), 0.0f);
        }
        if (m.t >= m.dur) {
            meteor_impact(m.to);
        }
    }
    std::erase_if(meteors_, [](const MeteorFx& m) { return m.t >= m.dur; });
    track_projectiles(); // a spell bolt that vanished / landed bursts where it struck
    update_shields(dt);  // Aegis pop-in / hit flash / shatter

    // Glowing trails behind in-flight bolts / arrows.
    if (have_snapshot_) {
        for (const net::ProjectileState& pr : snapshot_.projectiles) {
            if (projectile_spent(pr)) {
                continue; // already burst where it landed
            }
            const Vec3 back = -pr.dir;
            if (pr.kind == 2) { // holy bolt: mint motes + golden sparkles
                emit(pr.position, rand_dir() * 0.3f, Vec4{0.55f, 1.0f, 0.8f, 0.9f}, 0.45f, 0.16f, 1);
                emit(pr.position + rand_dir() * 0.2f, back * 2.0f + rand_dir() * 0.6f,
                     Vec4{1.0f, 0.92f, 0.6f, 0.9f}, 0.35f, 0.07f, 1);
            } else if (pr.kind == 4) { // arcane bolt: a swirling violet trail
                emit(pr.position, rand_dir() * 0.5f, Vec4{0.72f, 0.45f, 1.0f, 0.95f}, 0.45f, 0.15f, 1);
                emit(pr.position + rand_dir() * 0.18f, back * 1.0f, Vec4{0.88f, 0.75f, 1.0f, 0.8f}, 0.3f,
                     0.07f, 1);
            } else if (pr.kind == 5) { // fireball: a roaring tail of embers
                for (int e = 0; e < 3; ++e) {
                    emit_ember(pr.position + rand_dir() * 0.15f,
                               back * frand(1.0f, 3.0f) + rand_dir() * 0.8f + Vec3{0.0f, 0.6f, 0.0f},
                               ember_hot, ember_cool, frand(0.3f, 0.55f), frand(0.18f, 0.32f), -1.0f);
                }
            } else if (pr.kind == 6) { // frost bolt: icy motes + glittering shards
                emit(pr.position + rand_dir() * 0.1f, back * 1.0f + rand_dir() * 0.5f,
                     Vec4{0.7f, 0.9f, 1.0f, 0.8f}, 0.45f, 0.13f, 1, 0.5f);
                emit(pr.position + rand_dir() * 0.2f, rand_dir() * 0.6f, Vec4{0.92f, 0.98f, 1.0f, 1.0f}, 0.3f,
                     0.05f, 1, 2.0f);
            } else if (pr.kind == 7) { // boulder: an amber shimmer of the earth magic hurling it
                emit(pr.position + rand_dir() * 0.3f, back * 1.0f + rand_dir() * 0.4f,
                     Vec4{0.95f, 0.7f, 0.35f, 0.45f}, 0.35f, 0.12f, 1);
            } else if (pr.kind == 3) {
                emit(pr.position, Vec3{0.0f}, Vec4{0.85f, 0.95f, 0.7f, 0.5f}, 0.25f, 0.07f, 1);
            }
        }
        // Motes rising out of each ground aura, tinted by its kind (heal = gentle, drifting;
        // consecration = flickering holy-fire embers that climb faster and cool as they rise).
        for (const net::AuraState& a : snapshot_.auras) {
            const AuraProps props = aura_props(static_cast<AuraKind>(a.kind));
            const bool fire = static_cast<AuraKind>(a.kind) == AuraKind::Consecration;
            for (int i = 0; i < (fire ? 4 : 3); ++i) {
                const f32 ang = frand(0.0f, TwoPi);
                const f32 rr = a.radius * std::sqrt(frand());
                const Vec3 p = a.position + Vec3{std::cos(ang) * rr, 0.1f, std::sin(ang) * rr};
                if (fire) {
                    emit_ember(p, Vec3{0.0f, frand(1.4f, 3.2f), 0.0f}, Vec3{1.0f, 0.85f, 0.45f},
                               Vec3{0.9f, 0.3f, 0.08f}, frand(0.45f, 0.7f), 0.15f, -1.4f, 0.9f);
                } else {
                    emit(p, Vec3{0.0f, frand(0.9f, 2.0f), 0.0f}, Vec4{props.color, 0.88f}, 0.9f, 0.11f, 1,
                         -0.8f);
                }
            }
        }
        // Shimmer sparkles skating over each Aegis bubble's surface.
        auto sparkle = [&](const Vec3& feet, f32 strength) {
            if (strength > 0.02f) {
                for (int i = 0; i < 2; ++i) {
                    const Vec3 d = rand_dir();
                    emit(feet + Vec3{0.0f, 0.95f, 0.0f} + d * vfx::shield_radius, d * 0.2f,
                         Vec4{0.65f, 0.9f, 1.0f, 0.85f * strength}, 0.45f, 0.08f, 1);
                }
            }
        };
        for (const net::PlayerState& p : snapshot_.players) {
            sparkle(p.position, static_cast<f32>(p.shield) / 255.0f);
        }
        for (const net::VillagerState& v : snapshot_.villagers) {
            sparkle(v.position, static_cast<f32>(v.shield) / 255.0f);
        }
        // Motes of light drifting up inside each max-Aegis dome.
        for (const net::BubbleState& b : snapshot_.bubbles) {
            const f32 ang = frand(0.0f, TwoPi);
            const f32 rr = b.radius * 0.9f * std::sqrt(frand());
            emit(b.position + Vec3{std::cos(ang) * rr, 0.1f, std::sin(ang) * rr},
                 Vec3{0.0f, frand(0.6f, 1.4f), 0.0f}, Vec4{0.6f, 0.88f, 1.0f, 0.7f}, 1.4f, 0.1f, 1, -0.2f);
        }
    }
}

void ClientApp::draw_auras() {
    if (renderer_ == nullptr || !have_snapshot_) {
        return;
    }
    for (const net::AuraState& a : snapshot_.auras) {
        const AuraKind kind = static_cast<AuraKind>(a.kind);
        const AuraProps props = aura_props(kind);
        const f32 r = a.radius;
        const f32 breathe = 0.85f + 0.15f * std::sin(elapsed_ * 3.0f + a.position.x);
        // (shape_sphere_ is unit-DIAMETER, so a radius-r disc scales by 2r.)
        renderer_->draw_glow(shape_sphere_,
                             glm::translate(Mat4{1.0f}, a.position + Vec3{0.0f, 0.08f, 0.0f}) *
                                 glm::scale(Mat4{1.0f}, Vec3{r * 2.0f, 0.1f, r * 2.0f}),
                             Vec4{props.color, 0.2f * breathe}); // ground disc
        renderer_->draw_glow(shape_sphere_,
                             glm::translate(Mat4{1.0f}, a.position + Vec3{0.0f, 0.3f, 0.0f}) *
                                 glm::scale(Mat4{1.0f}, Vec3{r * 1.9f, r * 0.7f, r * 1.9f}),
                             Vec4{props.color, 0.05f}); // soft dome
        // A slowly turning rune circle marks the zone: a full sigil for the holy / healing ground,
        // a plain shimmering ring for the Hunter's caltrops.
        if (kind == AuraKind::Hazard) {
            sprite_circle(a.position + Vec3{0.0f, 0.12f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f},
                          r, 40, 0.06f, Vec4{props.color, 0.45f * breathe}, 0.6f);
        } else {
            draw_glyph(a.position, r, props.color, 0.7f * breathe, kind == AuraKind::Heal ? 5 : 6,
                       elapsed_ * 0.35f);
        }
        // It lights its surroundings day or night (strongest in the dark - see draw_fx_lights).
        if (props.light > 0.0f) {
            fx_light(a.position + Vec3{0.0f, 1.2f, 0.0f}, props.color, props.light * breathe, r * 2.2f);
        }
    }
}

void ClientApp::draw_shields() {
    if (renderer_ == nullptr || !have_snapshot_) {
        return;
    }
    auto bubble = [&](u64 key, const Vec3& feet, f32 strength) {
        if (strength <= 0.02f) {
            return;
        }
        f32 age = 1.0f;
        f32 hit = 0.0f;
        if (const auto it = shield_fx_.find(key); it != shield_fx_.end()) {
            age = it->second.age;
            hit = it->second.hit;
        }
        // Pops up with an overshoot (ease-out-back), then breathes gently.
        const f32 t = glm::clamp(age / 0.35f, 0.0f, 1.0f) - 1.0f;
        const f32 pop = std::max(1.0f + 2.70158f * t * t * t + 1.70158f * t * t, 0.05f);
        const f32 R = vfx::shield_radius * pop * (1.0f + 0.02f * std::sin(elapsed_ * 4.0f + feet.x));
        const Vec3 c = feet + Vec3{0.0f, 0.95f, 0.0f};
        const f32 glow = 0.55f + 0.45f * strength + hit; // brighter while strong, flashing when struck
        const Vec3 tint = glm::mix(Vec3{0.45f, 0.75f, 1.0f}, Vec3{0.85f, 0.95f, 1.0f}, hit);
        // A faceted translucent shell + a faint additive inner glow (shape_sphere_ is unit-DIAMETER).
        renderer_->draw_transparent(shape_sphere_,
                                    glm::translate(Mat4{1.0f}, c) * glm::scale(Mat4{1.0f}, Vec3{R * 2.0f}),
                                    Vec4{tint, 0.06f + 0.06f * strength + 0.12f * hit});
        renderer_->draw_glow(shape_sphere_,
                             glm::translate(Mat4{1.0f}, c) * glm::scale(Mat4{1.0f}, Vec3{R * 1.96f}),
                             Vec4{tint, 0.04f + 0.03f * strength + 0.15f * hit});
        sphere_rim(c, R, tint, glow, 40, 0.06f);
        // Two gyroscope rings sweeping round the shell.
        for (int k = 0; k < 2; ++k) {
            const f32 spin = elapsed_ * (k == 0 ? 0.9f : -0.7f) + static_cast<f32>(key % 97u) * 0.37f;
            const f32 tilt = k == 0 ? 0.5f : -0.9f;
            const Vec3 n = glm::normalize(
                Vec3{std::cos(spin) * std::sin(tilt), std::cos(tilt), std::sin(spin) * std::sin(tilt)});
            const Vec3 a = glm::normalize(glm::cross(n, Vec3{1.0f, 0.0f, 0.0f}));
            sprite_circle(c, a, glm::cross(n, a), R * 1.005f, 32, 0.035f, Vec4{tint, 0.22f * glow}, 0.8f);
        }
        // A halo where the bubble meets the ground.
        const f32 h = c.y - feet.y;
        if (R > h) {
            sprite_circle(feet + Vec3{0.0f, 0.1f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f},
                          std::sqrt(R * R - h * h), 32, 0.08f, Vec4{tint, 0.45f * glow}, 0.4f);
        }
        fx_light(c, Vec3{0.45f, 0.75f, 1.0f}, 1.4f * strength + 2.5f * hit, 6.5f);
    };
    for (const net::PlayerState& p : snapshot_.players) {
        bubble(static_cast<u64>(p.id), p.position, static_cast<f32>(p.shield) / 255.0f);
    }
    for (const net::VillagerState& v : snapshot_.villagers) {
        bubble((u64{1} << 32) | static_cast<u64>(v.id), v.position, static_cast<f32>(v.shield) / 255.0f);
    }
}

void ClientApp::draw_bubbles() {
    if (renderer_ == nullptr || !have_snapshot_) {
        return;
    }
    // The same glassy shell as the per-ally bubbles at full dome size, plus a slowly turning
    // geodesic cage and a great sigil where it meets the ground.
    for (const net::BubbleState& b : snapshot_.bubbles) {
        const f32 strength = static_cast<f32>(b.strength) / 255.0f;
        const Vec3 c = b.position + Vec3{0.0f, 1.0f, 0.0f};
        const f32 R = b.radius * (1.0f + 0.015f * std::sin(elapsed_ * 3.0f));
        const Vec3 tint{0.5f, 0.8f, 1.0f};
        const f32 glow = 0.5f + 0.5f * strength;
        renderer_->draw_transparent(shape_sphere_,
                                    glm::translate(Mat4{1.0f}, c) * glm::scale(Mat4{1.0f}, Vec3{R * 2.0f}),
                                    Vec4{0.45f, 0.72f, 1.0f, 0.05f + 0.07f * strength});
        renderer_->draw_glow(shape_sphere_,
                             glm::translate(Mat4{1.0f}, c) * glm::scale(Mat4{1.0f}, Vec3{R * 1.98f}),
                             Vec4{tint, 0.03f + 0.04f * strength});
        sphere_rim(c, R, tint, glow, 72, 0.1f);
        for (int m = 0; m < 6; ++m) { // meridians, turning slowly
            const f32 a = elapsed_ * 0.15f + Pi * static_cast<f32>(m) / 6.0f;
            sprite_circle(c, Vec3{std::cos(a), 0.0f, std::sin(a)}, Vec3{0.0f, 1.0f, 0.0f}, R, 40, 0.035f,
                          Vec4{tint, 0.12f * glow}, 0.7f);
        }
        for (const f32 lat : {0.35f, 0.7f}) { // latitude bands above the ground
            const f32 y = R * lat;
            sprite_circle(c + Vec3{0.0f, y, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f},
                          std::sqrt(R * R - y * y), 48, 0.035f, Vec4{tint, 0.12f * glow}, 0.7f);
        }
        const f32 h = c.y - b.position.y;
        if (R > h) {
            draw_glyph(b.position, std::sqrt(R * R - h * h), tint, 0.55f * glow, 6, elapsed_ * 0.2f);
        }
        fx_light(b.position + Vec3{0.0f, 2.0f, 0.0f}, tint, 2.2f * glow, R * 2.2f);
    }
}

void ClientApp::draw_particles() {
    if (renderer_ == nullptr) {
        return;
    }
    for (const Particle& p : particles_) {
        const f32 t = glm::clamp(p.life / p.max_life, 0.0f, 1.0f);
        const f32 sz = p.size * (0.35f + 0.65f * t);
        // Embers cool from their hot colour toward `tail` as they age.
        const Vec3 rgb = p.tail.x >= 0.0f ? glm::mix(p.tail, Vec3{p.color}, t) : Vec3{p.color};
        const Vec4 col{rgb, p.color.a * t};
        if (p.style == 1) {
            // A soft glow sprite (wider than the old sphere - the halo falls off to its rim) that
            // smears into a short streak along its velocity when it's moving fast (sparks).
            const f32 speed = glm::length(p.vel);
            const Vec3 tail = speed > 2.5f ? p.vel * std::min(0.04f, 1.0f / speed) : Vec3{0.0f};
            renderer_->draw_sprite(p.pos, p.pos - tail, sz * 1.25f, col, 0.55f);
        } else {
            renderer_->draw_emissive(shape_sphere_,
                                     glm::translate(Mat4{1.0f}, p.pos) * glm::scale(Mat4{1.0f}, Vec3{sz}),
                                     col);
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

void ClientApp::update_fish(Timestep dt) {
    if (renderer_ == nullptr || world_seed_ == 0) {
        return;
    }
    const Vec3 feet = local_feet();
    auto ground = [&](f32 x, f32 z) { return worldgen::height(x, z, world_seed_); };
    // The biome around the player decides the fish look: warm seas get bright tropical fish, cold /
    // freshwater gets silvery / dark ones.
    const auto biome = worldgen::biome_at(feet.x, feet.z, world_seed_);
    const bool warm = worldgen::temperature(feet.x, feet.z, world_seed_) > 0.46f &&
                      (biome == worldgen::Biome::Ocean || biome == worldgen::Biome::Beach);
    static const Vec3 tropical[] = {{1.0f, 0.55f, 0.18f}, {1.0f, 0.82f, 0.25f}, {0.30f, 0.62f, 1.0f},
                                    {0.95f, 0.42f, 0.55f}, {0.45f, 0.85f, 0.78f}};
    static const Vec3 temperate[] = {{0.78f, 0.80f, 0.86f}, {0.55f, 0.62f, 0.60f}, {0.62f, 0.56f, 0.42f},
                                     {0.70f, 0.74f, 0.82f}};

    // Spawn a shoal in the water near the player. Each fish wants a wet spot (real water, a little
    // below the waterline) within view; if none is found in a few tries we just hold the count.
    auto wet_spot = [&](Vec3& out) {
        for (int tries = 0; tries < 6; ++tries) {
            const f32 a = frand(0.0f, TwoPi), r = frand(8.0f, 26.0f);
            const Vec3 p{feet.x + std::cos(a) * r, 0.0f, feet.z + std::sin(a) * r};
            const f32 g = ground(p.x, p.z);
            if (g < worldgen::water_level - 0.5f) { // genuinely under water (not just a damp shore)
                out = Vec3{p.x, glm::max(g + 0.25f, worldgen::water_level - frand(0.25f, 0.9f)), p.z};
                return true;
            }
        }
        return false;
    };
    int guard = 0;
    while (fish_.size() < 7 && guard++ < 4) {
        Fish f;
        if (!wet_spot(f.pos)) {
            break; // no water nearby - no fish this frame
        }
        f.yaw = frand(0.0f, TwoPi);
        f.target = f.pos;
        f.scale = frand(0.6f, 1.3f);
        f.tint = warm ? tropical[static_cast<int>(frand(0.0f, 5.0f)) % 5]
                      : temperate[static_cast<int>(frand(0.0f, 4.0f)) % 4];
        fish_.push_back(f);
    }

    for (Fish& f : fish_) {
        const f32 dist = glm::length(Vec2{f.pos.x - feet.x, f.pos.z - feet.z});
        const f32 g = ground(f.pos.x, f.pos.z);
        if (dist > 34.0f || g > worldgen::water_level - 0.3f) {
            // wandered out of range or beached - respawn at a fresh wet spot (or cull if no water).
            if (!wet_spot(f.pos)) {
                f.pos.y = -1e6f; // mark for removal
            }
            f.retarget = 0.0f;
            f.darting = false;
            continue;
        }
        f.darting = dist < 6.0f; // dart away if the player wades close
        f.retarget -= dt.seconds;
        if (f.darting) {
            const Vec2 away = glm::normalize(Vec2{f.pos.x - feet.x, f.pos.z - feet.z} + Vec2{1e-3f});
            f.target = f.pos + Vec3{away.x, 0.0f, away.y} * 8.0f;
        } else if (f.retarget <= 0.0f) {
            const f32 a = frand(0.0f, TwoPi), r = frand(2.0f, 7.0f);
            f.target = f.pos + Vec3{std::cos(a) * r, 0.0f, std::sin(a) * r};
            f.retarget = frand(1.5f, 4.0f);
        }
        const Vec2 to{f.target.x - f.pos.x, f.target.z - f.pos.z};
        const f32 td = glm::length(to);
        const f32 spd = (f.darting ? 7.0f : 1.8f) * dt.seconds;
        if (td > 0.1f) {
            const Vec2 step = to / td * std::min(spd, td);
            const f32 ng = ground(f.pos.x + step.x, f.pos.z + step.y);
            if (ng < worldgen::water_level - 0.2f) { // only swim where it stays wet
                f.pos.x += step.x;
                f.pos.z += step.y;
                f.yaw = std::atan2(step.y, step.x);
            } else {
                f.retarget = 0.0f; // hit the bank - pick a new heading
            }
            // hold just under the surface, above the bed
            f.pos.y = glm::clamp(f.pos.y, ground(f.pos.x, f.pos.z) + 0.2f, worldgen::water_level - 0.12f);
            f.wiggle += glm::length(step) * 6.0f + dt.seconds * (f.darting ? 18.0f : 7.0f);
        } else {
            f.wiggle += dt.seconds * 6.0f;
        }
    }
    std::erase_if(fish_, [](const Fish& f) { return f.pos.y < -1e5f; });
}

void ClientApp::draw_fish() {
    if (renderer_ == nullptr) {
        return;
    }
    for (const Fish& f : fish_) {
        // A swimming wiggle: oscillate the heading a touch so the body + tail sashay side to side.
        const f32 wob = std::sin(f.wiggle) * (f.darting ? 0.45f : 0.28f);
        const Mat4 m = glm::translate(Mat4{1.0f}, f.pos) *
                       glm::rotate(Mat4{1.0f}, -(f.yaw + wob), Vec3{0.0f, 1.0f, 0.0f}) *
                       glm::scale(Mat4{1.0f}, Vec3{f.scale});
        renderer_->draw(fish_body_mesh_, m, Vec4{f.tint, 1.0f});
    }
}

void ClientApp::draw_surf() {
    if (renderer_ == nullptr || world_seed_ == 0) {
        return;
    }
    const Vec3 feet = local_feet();
    const f32 t = elapsed_;
    auto ground = [&](f32 x, f32 z) { return worldgen::height(x, z, world_seed_); };
    auto hcell = [](int x, int z, int s) {
        u32 v = static_cast<u32>(x * 73856093) ^ static_cast<u32>(z * 19349663) ^
                static_cast<u32>(s * 83492791);
        v ^= v >> 13;
        v *= 0x2545F491u;
        v ^= v >> 16;
        return static_cast<f32>((v >> 8) & 0xFFFFu) / 65535.0f;
    };
    // Surf foam hugging the shore. Scan WORLD cells around the player; for cells right on the
    // waterline (water on one side, land on the other) lay a thin, soft foam STREAK oriented ALONG
    // the shore (parallel to the waterline, perpendicular to the slope), so adjacent cells line up
    // into a broken foam line that follows the coast - not big discs/blobs. The streak's brightness
    // rides a wave that rolls along the shore and it laps in/out a touch along the slope, so the
    // surf shimmers and washes. World-anchored, so it stays put on the beach as the camera moves.
    constexpr f32 cell = 1.5f, radius = 24.0f;
    const int bcx = static_cast<int>(std::floor(feet.x / cell));
    const int bcz = static_cast<int>(std::floor(feet.z / cell));
    const int cr = static_cast<int>(radius / cell) + 1;
    for (int dz = -cr; dz <= cr; ++dz) {
        for (int dx = -cr; dx <= cr; ++dx) {
            const int cx = bcx + dx, cz = bcz + dz;
            const f32 wx = static_cast<f32>(cx) * cell + cell * 0.5f;
            const f32 wz = static_cast<f32>(cz) * cell + cell * 0.5f;
            const f32 gh = ground(wx, wz);
            if (gh < worldgen::water_level - 0.22f || gh > worldgen::water_level + 0.2f) {
                continue; // a tight band right at the waterline
            }
            const f32 hl = ground(wx - cell, wz), hr = ground(wx + cell, wz);
            const f32 hu = ground(wx, wz - cell), hd = ground(wx, wz + cell);
            const f32 lo = std::min(std::min(hl, hr), std::min(hu, hd));
            const f32 hi = std::max(std::max(hl, hr), std::max(hu, hd));
            if (lo > worldgen::water_level - 0.18f || hi < worldgen::water_level + 0.04f) {
                continue; // needs both water + land around it to be a real shore
            }
            const Vec2 grad{hr - hl, hd - hu};
            if (glm::length(grad) < 1e-3f) {
                continue; // need a real slope to know which way the shore runs
            }
            const Vec2 up_slope = glm::normalize(grad);          // uphill = toward land
            const Vec2 tangent{up_slope.y, -up_slope.x};         // along the shore (the foam line)
            const f32 dist = glm::length(Vec2{wx - feet.x, wz - feet.z});
            if (dist > radius) {
                continue;
            }
            const f32 fade = glm::smoothstep(radius, radius * 0.5f, dist);
            // Wave rolls along the shore; foam brightens near the crest and laps in/out a touch.
            const f32 along = glm::dot(Vec2{wx, wz}, tangent);
            const f32 wave = 0.5f + 0.5f * std::sin(t * 1.4f + along * 0.5f);
            const f32 crest = glm::smoothstep(0.2f, 1.0f, wave);
            const f32 a = fade * (0.4f + 0.45f * crest); // a clear foam line, brightest at the crest
            if (a < 0.04f) {
                continue;
            }
            const Vec2 p = Vec2{wx, wz} + up_slope * (crest * 0.4f - 0.1f); // laps at the waterline
            const f32 ang = std::atan2(-tangent.y, tangent.x);   // local +X -> shore tangent
            const f32 len = 1.5f + hcell(cx, cz, 7) * 0.7f;      // a streak spanning ~the cell
            const f32 wid = 0.26f + crest * 0.18f;               // thin across, swells a touch
            // A flat single-sided up-quad (NOT a box): a flattened solid would show its dark
            // underside in the no-depth-write transparent pass, rendering the foam black.
            const Mat4 m = glm::translate(Mat4{1.0f}, Vec3{p.x, worldgen::water_level + 0.05f, p.y}) *
                           glm::rotate(Mat4{1.0f}, ang, Vec3{0.0f, 1.0f, 0.0f}) *
                           glm::scale(Mat4{1.0f}, Vec3{len, 1.0f, wid});
            renderer_->draw_transparent(shape_quad_, m, Vec4{0.95f, 0.98f, 1.0f, a});
        }
    }
}

void ClientApp::spawn_ability_vfx(PlayerRole role, u8 slot, const Vec3& feet, f32 yaw, const Vec3& aim,
                                  u8 rank) {
    const Vec3 chest = feet + Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 facing{std::cos(yaw), 0.0f, std::sin(yaw)};
    const Vec3 staff = feet + Vec3{0.0f, 1.45f, 0.0f} + facing * 0.45f; // a raised staff-head / hand
    Vec3 to_aim = aim - chest;
    to_aim.y = 0.0f;
    const Vec3 fwd = glm::length(to_aim) > 0.3f ? glm::normalize(to_aim) : facing;
    // A rising column of `n` motes from a disc of radius `rad` at the feet (a beam of light shooting up).
    auto pillar = [&](const Vec4& col, int n, f32 rad, f32 speed) {
        for (int i = 0; i < n; ++i) {
            const f32 a = frand(0.0f, TwoPi), rr = frand(0.0f, rad);
            emit(feet + Vec3{std::cos(a) * rr, frand(0.0f, 0.3f), std::sin(a) * rr},
                 Vec3{0.0f, frand(speed * 0.6f, speed), 0.0f}, col, 0.9f, 0.14f, 1, -1.0f);
        }
    };
    // A spinning ring of motes launched tangentially (a swirling disc), radius `rad` around the feet.
    auto swirl = [&](const Vec4& col, int n, f32 rad, f32 height, f32 tang) {
        for (int i = 0; i < n; ++i) {
            const f32 a = TwoPi * static_cast<f32>(i) / static_cast<f32>(n);
            const Vec3 p = feet + Vec3{std::cos(a) * rad, height, std::sin(a) * rad};
            emit(p, Vec3{-std::sin(a), 0.15f, std::cos(a)} * tang, col, 0.5f, 0.14f, 1);
        }
    };
    // Every other player within `r` of the caster (the allies a group ability links to).
    auto for_allies = [&](f32 r, auto&& fn) {
        if (!have_snapshot_) {
            return;
        }
        for (const net::PlayerState& p : snapshot_.players) {
            const f32 d = glm::length(p.position - feet);
            if (d > 0.5f && d <= r) {
                fn(p);
            }
        }
    };
    // The most wounded player within `radius` of `from` below `below` % health, skipping `skip` -
    // who a Cleric's mend lands on (the server picks the same way, the caster included).
    auto most_hurt = [&](const Vec3& from, f32 radius, const std::vector<u32>& skip,
                         int below) -> const net::PlayerState* {
        const net::PlayerState* best = nullptr;
        int worst = below;
        if (have_snapshot_) {
            for (const net::PlayerState& p : snapshot_.players) {
                if (glm::length(p.position - from) > radius ||
                    std::find(skip.begin(), skip.end(), p.id) != skip.end()) {
                    continue;
                }
                if (static_cast<int>(p.health) < worst) {
                    worst = static_cast<int>(p.health);
                    best = &p;
                }
            }
        }
        return best;
    };
    switch (role) {
        case PlayerRole::Knight:
            if (slot == 0) { // Shield Bash: a steel shockwave punched forward + a ground impact ring
                const Vec3 c = chest + fwd * 1.2f;
                emit(c, Vec3{0.0f}, Vec4{0.95f, 0.98f, 1.0f, 1.0f}, 0.2f, 0.85f, 1);
                emit_ring(c - Vec3{0.0f, 0.9f, 0.0f}, Vec4{0.8f, 0.88f, 1.0f, 0.9f}, 26, 5.0f, 0.4f, 0.15f);
                for (int i = 0; i < 44; ++i) {
                    const f32 a = yaw + frand(-0.7f, 0.7f);
                    const Vec3 d{std::cos(a), frand(0.0f, 0.6f), std::sin(a)};
                    emit(c, d * frand(7.0f, 14.0f), Vec4{0.82f, 0.9f, 1.0f, 0.95f}, 0.42f, 0.17f, 1, 6.0f);
                }
                flash_light(c, Vec3{0.8f, 0.88f, 1.0f}, 3.5f, 8.0f, 0.25f);
            } else if (slot == 1 || slot == 5) { // Bulwark / Rally: a golden sigil + a rising light column
                glyph(feet, 1.6f, Vec4{1.0f, 0.82f, 0.36f, 1.0f}, 1.0f, 6);
                emit_ring(feet, Vec4{1.0f, 0.85f, 0.4f, 0.95f}, 30, 3.2f, 0.6f, 0.17f);
                pillar(Vec4{1.0f, 0.82f, 0.32f, 0.9f}, 26, 0.7f, 4.5f);
                emit(chest, Vec3{0.0f}, Vec4{1.0f, 0.9f, 0.5f, 1.0f}, 0.22f, 0.7f, 1);
                if (slot == 5) { // Rally: golden mending links to every ally in reach
                    for_allies(kHealRadius, [&](const net::PlayerState& p) {
                        beam(chest + Vec3{0.0f, 0.4f, 0.0f}, p.position + Vec3{0.0f, 1.0f, 0.0f},
                             Vec4{1.0f, 0.85f, 0.4f, 0.9f}, 0.06f, 0.5f, 0, 40.0f);
                    });
                }
                flash_light(chest, Vec3{1.0f, 0.82f, 0.4f}, 3.5f, 8.0f, 0.5f);
            } else if (slot == 2) { // Consecration: a holy-fire sigil + flame columns erupt from the ground
                glyph(feet, kConsecrationRadius, Vec4{1.0f, 0.65f, 0.25f, 1.0f}, 1.0f, 6);
                emit_ring(feet, Vec4{1.0f, 0.72f, 0.28f, 0.95f}, 34, kConsecrationRadius * 1.6f, 0.6f, 0.2f);
                for (int k = 0; k < 10; ++k) {
                    const f32 a = TwoPi * static_cast<f32>(k) / 10.0f;
                    const Vec3 base = feet + Vec3{std::cos(a), 0.0f, std::sin(a)} * (kConsecrationRadius * 0.8f);
                    for (int j = 0; j < 3; ++j) {
                        emit_ember(base + Vec3{0.0f, 0.15f, 0.0f},
                                   Vec3{frand(-0.3f, 0.3f), frand(3.0f, 5.5f), frand(-0.3f, 0.3f)},
                                   Vec3{1.0f, 0.88f, 0.5f}, Vec3{0.9f, 0.3f, 0.08f}, frand(0.5f, 0.7f), 0.22f,
                                   -1.5f);
                    }
                }
                pillar(Vec4{1.0f, 0.6f, 0.2f, 0.9f}, 20, kConsecrationRadius * 0.7f, 3.0f);
                flash_light(feet + Vec3{0.0f, 1.5f, 0.0f}, Vec3{1.0f, 0.6f, 0.22f}, 5.0f, 12.0f, 0.7f);
            } else if (slot == 4) { // Whirlwind: three counter-swirling steel rings sweep round the knight
                swirl(Vec4{0.88f, 0.92f, 1.0f, 0.95f}, 22, kWhirlwindRadius * 1.1f, 0.7f, 7.0f);
                swirl(Vec4{0.8f, 0.86f, 1.0f, 0.9f}, 18, kWhirlwindRadius * 0.7f, 1.2f, -6.0f);
                swirl(Vec4{0.95f, 0.97f, 1.0f, 0.8f}, 16, kWhirlwindRadius * 0.9f, 1.0f, 9.0f);
                emit(chest, Vec3{0.0f}, Vec4{0.9f, 0.95f, 1.0f, 1.0f}, 0.16f, 0.5f, 1);
                flash_light(chest, Vec3{0.85f, 0.9f, 1.0f}, 2.5f, 7.0f, 0.3f);
            } else if (slot == 3) { // Taunt: a red warcry shockwave + upward embers + a ground glyph
                glyph(feet, 2.0f, Vec4{1.0f, 0.3f, 0.22f, 1.0f}, 0.8f, 5);
                emit(chest, Vec3{0.0f}, Vec4{1.0f, 0.35f, 0.25f, 1.0f}, 0.2f, 0.8f, 1);
                emit_ring(feet, Vec4{1.0f, 0.3f, 0.25f, 0.95f}, 32, 8.0f, 0.55f, 0.19f);
                emit_ring(feet, Vec4{1.0f, 0.5f, 0.3f, 0.8f}, 22, 4.5f, 0.7f, 0.16f);
                pillar(Vec4{1.0f, 0.4f, 0.28f, 0.9f}, 20, 0.8f, 3.5f);
                flash_light(chest, Vec3{1.0f, 0.3f, 0.22f}, 3.5f, 9.0f, 0.45f);
            } else { // Guardian Leap: a golden launch burst + a shield flare
                emit_ring(feet, Vec4{1.0f, 0.86f, 0.42f, 0.95f}, 28, 4.0f, 0.5f, 0.16f);
                emit_burst(chest, Vec4{1.0f, 0.9f, 0.5f, 0.95f}, 28, 6.0f, 0.5f, 0.15f, 1, 4.0f);
                flash_light(chest, Vec3{1.0f, 0.85f, 0.45f}, 3.5f, 8.0f, 0.4f);
            }
            break;
        case PlayerRole::Hunter:
            if (slot == 2) { // Dash: a green speed-burst behind + a forward launch streak
                emit_burst(feet + Vec3{0.0f, 0.4f, 0.0f}, Vec4{0.6f, 1.0f, 0.6f, 0.9f}, 26, -4.5f, 0.5f, 0.13f, 1);
                for (int i = 0; i < 20; ++i) {
                    emit(chest - facing * frand(0.0f, 1.0f), -facing * frand(2.0f, 6.0f),
                         Vec4{0.7f, 1.0f, 0.7f, 0.8f}, 0.4f, 0.12f, 1);
                }
                flash_light(chest, Vec3{0.6f, 1.0f, 0.6f}, 1.8f, 6.0f, 0.3f);
            } else if (slot == 5) { // Caltrops: a scatter ring + little spikes flung up at the aim
                emit_ring(aim, Vec4{0.9f, 0.82f, 0.3f, 0.9f}, 28, kHazardRadius * 1.3f, 0.5f, 0.13f);
                for (int i = 0; i < 20; ++i) {
                    emit(aim + Vec3{frand(-0.6f, 0.6f), 0.1f, frand(-0.6f, 0.6f)},
                         Vec3{frand(-1.0f, 1.0f), frand(2.0f, 4.0f), frand(-1.0f, 1.0f)},
                         Vec4{0.9f, 0.85f, 0.35f, 0.9f}, 0.5f, 0.1f, 1, 8.0f);
                }
                flash_light(aim + Vec3{0.0f, 0.6f, 0.0f}, Vec3{0.9f, 0.8f, 0.35f}, 1.8f, 6.0f, 0.3f);
            } else if (slot == 6) { // War Horn: rolling waves of sound in green-gold + a rallying light
                for (int k = 0; k < 3; ++k) {
                    emit_ring(feet + Vec3{0.0f, 0.3f * static_cast<f32>(k), 0.0f},
                              Vec4{0.7f, 1.0f, 0.5f, 0.85f - 0.2f * static_cast<f32>(k)}, 36,
                              kHasteRadius * (0.6f + 0.3f * static_cast<f32>(k)), 0.7f, 0.16f);
                }
                emit_burst(chest, Vec4{0.85f, 1.0f, 0.55f, 0.9f}, 20, 3.5f, 0.6f, 0.13f, 1, 2.0f);
                glyph(feet, 1.8f, Vec4{0.6f, 1.0f, 0.5f, 0.9f}, 0.9f, 5);
                flash_light(chest, Vec3{0.6f, 1.0f, 0.5f}, 3.0f, 10.0f, 0.6f);
            } else { // Power / Volley / Multishot / Piercing: a bright muzzle flash + spray along the shot
                const Vec3 c = chest + fwd * 0.8f;
                emit(c, Vec3{0.0f}, Vec4{0.85f, 1.0f, 0.7f, 1.0f}, 0.15f, 0.5f, 1);
                const bool wide = slot == 1 || slot == 4; // Volley / Multishot fan wider
                const f32 spread = wide ? 0.45f : (slot == 3 ? 0.06f : 0.15f); // Piercing = tight line
                for (int i = 0; i < (wide ? 32 : 18); ++i) {
                    const f32 a = std::atan2(fwd.z, fwd.x) + frand(-spread, spread);
                    emit(c, Vec3{std::cos(a), frand(-0.1f, 0.2f), std::sin(a)} * frand(6.0f, 13.0f),
                         Vec4{0.72f, 1.0f, 0.6f, 0.9f}, 0.35f, 0.12f, 1);
                }
                if (!wide) { // the heavy single shots tear a short glowing wake through the air
                    beam(c, c + fwd * 5.0f, Vec4{0.75f, 1.0f, 0.6f, 0.9f}, 0.05f, 0.2f, 0, 70.0f);
                }
                flash_light(c, Vec3{0.7f, 1.0f, 0.6f}, 2.0f, 6.0f, 0.2f);
            }
            break;
        case PlayerRole::Cleric:
            if (slot == 0) {
                // HEAL: a shaft of light from the sky onto whoever it mends, plus a ray from the staff
                // when that's an ally; at max rank the mend CHAINS on - crackling arcs hop to the next
                // wounded allies in reach.
                const net::PlayerState* tgt = most_hurt(feet, kHealRadius, {}, 101);
                const Vec3 at = tgt != nullptr ? tgt->position : feet;
                beam(at + Vec3{0.0f, 9.0f, 0.0f}, at, Vec4{0.6f, 1.0f, 0.75f, 0.85f}, 0.18f, 0.6f, 2);
                for (int i = 0; i < 26; ++i) {
                    emit(at + Vec3{frand(-0.5f, 0.5f), frand(0.1f, 0.4f), frand(-0.5f, 0.5f)},
                         Vec3{frand(-0.3f, 0.3f), frand(1.8f, 3.4f), frand(-0.3f, 0.3f)},
                         Vec4{0.55f, 1.0f, 0.7f, 0.9f}, 0.8f, 0.12f, 1, -1.2f);
                }
                emit_ring(at, Vec4{0.55f, 1.0f, 0.7f, 0.8f}, 20, 2.0f, 0.5f, 0.13f);
                if (tgt != nullptr && glm::length(tgt->position - feet) > 0.5f) {
                    beam(staff, at + Vec3{0.0f, 1.0f, 0.0f}, Vec4{0.6f, 1.0f, 0.7f, 1.0f}, 0.07f, 0.5f, 0, 45.0f);
                }
                if (rank >= kMaxAbilityRank && tgt != nullptr) {
                    std::vector<u32> healed{tgt->id};
                    const net::PlayerState* from = tgt;
                    for (int hop = 0; hop < kChainHealBounces; ++hop) {
                        const net::PlayerState* next = most_hurt(from->position, kChainHealRadius, healed, 100);
                        if (next == nullptr) {
                            break; // no wounded ally left in reach - the chain fizzles
                        }
                        beam(from->position + Vec3{0.0f, 1.0f, 0.0f}, next->position + Vec3{0.0f, 1.0f, 0.0f},
                             Vec4{0.7f, 1.0f, 0.6f, 0.9f}, 0.06f, 0.6f, 1);
                        emit_burst(next->position + Vec3{0.0f, 0.9f, 0.0f}, Vec4{0.6f, 1.0f, 0.7f, 0.9f}, 12,
                                   2.5f, 0.6f, 0.12f, 1, 1.5f);
                        healed.push_back(next->id);
                        from = next;
                    }
                }
                flash_light(at + Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.55f, 1.0f, 0.7f}, 3.0f, 8.0f, 0.6f);
            } else if (slot == 1) { // Sanctuary: a wide sigil, a radiant ring, a great column + links
                glyph(feet, 2.6f, Vec4{0.6f, 1.0f, 0.8f, 1.0f}, 1.2f, 6);
                emit_ring(feet, Vec4{0.6f, 1.0f, 0.8f, 0.95f}, 48, 8.0f, 0.8f, 0.2f);
                pillar(Vec4{0.72f, 1.0f, 0.86f, 0.9f}, 36, 1.4f, 4.5f);
                beam(feet + Vec3{0.0f, 12.0f, 0.0f}, feet, Vec4{0.7f, 1.0f, 0.85f, 0.8f}, 0.45f, 0.9f, 2);
                for_allies(kHealRadius, [&](const net::PlayerState& p) {
                    beam(staff, p.position + Vec3{0.0f, 1.0f, 0.0f}, Vec4{0.65f, 1.0f, 0.8f, 0.9f}, 0.06f, 0.6f,
                         0, 45.0f);
                    emit_burst(p.position + Vec3{0.0f, 0.9f, 0.0f}, Vec4{0.65f, 1.0f, 0.8f, 0.9f}, 14, 2.5f, 0.6f,
                               0.12f, 1, 1.5f);
                });
                flash_light(feet + Vec3{0.0f, 2.0f, 0.0f}, Vec3{0.6f, 1.0f, 0.8f}, 5.0f, 14.0f, 1.0f);
            } else if (slot == 2 || slot == 5) { // Smite / Judgement: a pillar of holy light strikes the aim
                const bool judge = slot == 5;
                emit(staff, Vec3{0.0f}, Vec4{0.9f, 1.0f, 0.92f, 1.0f}, 0.22f, 0.7f, 1);
                beam(aim + Vec3{0.0f, 14.0f, 0.0f}, aim, Vec4{0.85f, 1.0f, 0.9f, 1.0f}, judge ? 0.3f : 0.2f, 0.5f, 2);
                glyph(aim, judge ? 1.8f : 1.3f, Vec4{0.85f, 1.0f, 0.88f, 0.9f}, 0.6f, judge ? 8 : 6);
                for (int i = 0; i < 20; ++i) { // motes raining down the pillar onto the aim point
                    emit(aim + Vec3{frand(-0.3f, 0.3f), frand(2.0f, 4.5f), frand(-0.3f, 0.3f)},
                         Vec3{0.0f, -frand(5.0f, 9.0f), 0.0f}, Vec4{0.85f, 1.0f, 0.9f, 0.95f}, 0.4f, 0.13f, 1);
                }
                emit_ring(aim, Vec4{0.8f, 1.0f, 0.88f, 0.9f}, 18, 3.0f, 0.35f, 0.13f);
                flash_light(aim + Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.75f, 1.0f, 0.85f}, judge ? 5.0f : 3.5f, 10.0f,
                            0.5f);
            } else if (slot == 3) { // Aegis: a cyan ward flare (the bubble itself pops in update_shields)
                emit(staff, Vec3{0.0f}, Vec4{0.55f, 0.85f, 1.0f, 1.0f}, 0.22f, 0.6f, 1);
                glyph(feet, 1.5f, Vec4{0.55f, 0.85f, 1.0f, 1.0f}, 0.8f, 6);
                emit_burst(chest, Vec4{0.6f, 0.9f, 1.0f, 0.9f}, 20, 2.8f, 0.6f, 0.13f, 1, 1.6f);
                flash_light(chest, Vec3{0.5f, 0.8f, 1.0f}, 3.0f, 8.0f, 0.4f);
            } else if (slot == 4) { // Renew: a lingering heal aura laid at the feet (it draws its own sigil)
                emit_ring(feet, Vec4{0.5f, 1.0f, 0.65f, 0.9f}, 30, kHealAuraRadius * 1.2f, 0.7f, 0.16f);
                pillar(Vec4{0.6f, 1.0f, 0.72f, 0.85f}, 24, kHealAuraRadius * 0.7f, 2.6f);
                flash_light(feet + Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.5f, 1.0f, 0.65f}, 3.0f, 10.0f, 0.6f);
            } else { // Empower: a crackling fiery link to the ally it blesses (or a blaze on yourself)
                const net::PlayerState* ally = nullptr;
                f32 best = kEmpowerRange;
                for_allies(kEmpowerRange, [&](const net::PlayerState& p) {
                    const f32 d = glm::length(p.position - feet);
                    if (d < best) {
                        best = d;
                        ally = &p;
                    }
                });
                const Vec3 at = ally != nullptr ? ally->position : feet;
                if (ally != nullptr) {
                    beam(staff, at + Vec3{0.0f, 1.0f, 0.0f}, Vec4{1.0f, 0.6f, 0.22f, 1.0f}, 0.08f, 0.6f, 1);
                }
                emit_ring(at, Vec4{1.0f, 0.55f, 0.22f, 0.9f}, 26, 3.0f, 0.6f, 0.15f);
                emit_burst(at + Vec3{0.0f, 1.0f, 0.0f}, Vec4{1.0f, 0.65f, 0.3f, 0.9f}, 24, 3.0f, 0.6f, 0.14f, 1, 2.0f);
                flash_light(at + Vec3{0.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.6f, 0.22f}, 3.5f, 8.0f, 0.5f);
            }
            break;
        case PlayerRole::Mage:
            break; // Mage spells play spawn_spell_vfx (the hotbar only queues elements)
    }
}

void ClientApp::update_ability_vfx(Timestep dt) {
    const Vec3 feet = local_feet();
    if (bulwark_fx_ > 0.0f) { // a golden shield dome orbiting the local Knight
        bulwark_fx_ -= dt.seconds;
        const f32 a = elapsed_ * 5.0f;
        for (int i = 0; i < 4; ++i) {
            const f32 ang = a + TwoPi * static_cast<f32>(i) / 4.0f;
            emit(feet + Vec3{std::cos(ang) * 0.9f, 0.9f + 0.4f * std::sin(a * 0.7f), std::sin(ang) * 0.9f},
                 Vec3{0.0f}, Vec4{1.0f, 0.82f, 0.35f, 0.7f}, 0.3f, 0.13f, 1);
        }
        fx_light(feet + Vec3{0.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.82f, 0.4f}, 1.2f, 5.0f);
    }
    if (dash_fx_ > 0.0f) { // a green speed-trail behind the local Hunter
        dash_fx_ -= dt.seconds;
        emit(feet + Vec3{0.0f, 0.5f, 0.0f}, Vec3{0.0f}, Vec4{0.6f, 1.0f, 0.6f, 0.6f}, 0.35f, 0.12f, 1);
    }
    // Cleric heal channel (right mouse held): mirror the server's charge for a charge bar +
    // gathering VFX; a burst of green motes converges on the staff, and on a full charge it
    // releases (resets) - the actual aura is server-spawned + networked.
    if (role_ == PlayerRole::Cleric && blocking_) {
        heal_charge_fx_ += dt.seconds;
        const Vec3 head = feet + Vec3{0.0f, 1.5f, 0.0f};
        const f32 charge = glm::clamp(heal_charge_fx_ / kHealChargeTime, 0.0f, 1.0f);
        for (int i = 0; i < 4; ++i) {
            const Vec3 from = head + rand_dir() * frand(1.0f, 2.2f);
            emit(from, (head - from) * frand(2.0f, 4.0f), Vec4{0.5f, 1.0f, 0.7f, 0.9f}, 0.4f, 0.1f, 1);
        }
        fx_light(head, Vec3{0.5f, 1.0f, 0.7f}, 0.5f + 2.0f * charge, 4.0f + 3.0f * charge);
        if (heal_charge_fx_ >= kHealChargeTime) {
            emit_ring(feet, Vec4{0.5f, 1.0f, 0.7f, 0.95f}, 32, 5.0f, 0.7f, 0.18f);
            emit_burst(feet + Vec3{0.0f, 0.3f, 0.0f}, Vec4{0.6f, 1.0f, 0.8f, 0.9f}, 24, 3.0f, 0.8f, 0.14f, 1, 2.0f);
            flash_light(head, Vec3{0.5f, 1.0f, 0.7f}, 4.0f, 10.0f, 0.6f);
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
        // Where they aimed rides along with the cast (fall back to just ahead of them).
        const Vec3 facing{std::cos(p.yaw), 0.0f, std::sin(p.yaw)};
        const Vec3 aim = glm::length(p.cast_aim) > 0.01f ? p.cast_aim : p.position + facing * 6.0f;
        const auto role = static_cast<PlayerRole>(p.role % kRoleCount);
        if (role == PlayerRole::Mage) {
            spawn_spell_vfx(static_cast<SpellId>(p.cast), p.position, p.yaw, aim); // a Mage casts a SpellId
        } else {
            const u8 slot = static_cast<u8>(p.cast - 1);
            spawn_ability_vfx(role, slot, p.position, p.yaw, aim,
                              static_cast<u8>((p.ability_ranks >> (2 * slot)) & 0x3u));
        }
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
        if ((p.buffs & 1u) != 0u) { // empowered: a fiery ring + rising embers + a warm glow
            renderer_->draw_glow(shape_cylinder_,
                                 glm::translate(Mat4{1.0f}, feet + Vec3{0.0f, 0.06f, 0.0f}) *
                                     glm::scale(Mat4{1.0f}, Vec3{1.15f, 0.05f, 1.15f}),
                                 Vec4{1.0f, 0.45f, 0.15f, 0.5f * pulse});
            sprite_circle(feet + Vec3{0.0f, 0.12f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, 0.65f,
                          20, 0.05f, Vec4{1.0f, 0.55f, 0.2f, 0.6f * pulse}, 0.7f);
            const f32 a = frand(0.0f, TwoPi);
            emit_ember(feet + Vec3{std::cos(a) * 0.6f, 0.15f, std::sin(a) * 0.6f},
                       Vec3{0.0f, frand(1.5f, 2.8f), 0.0f}, Vec3{1.0f, 0.8f, 0.4f}, Vec3{0.85f, 0.2f, 0.05f},
                       0.5f, 0.1f, -0.8f);
            fx_light(feet + Vec3{0.0f, 0.8f, 0.0f}, Vec3{1.0f, 0.5f, 0.18f}, 0.9f * pulse, 4.0f);
        }
        if ((p.buffs & 2u) != 0u) { // hasted: a green ring + wind streaks whipping round the legs
            renderer_->draw_glow(shape_cylinder_,
                                 glm::translate(Mat4{1.0f}, feet + Vec3{0.0f, 0.13f, 0.0f}) *
                                     glm::scale(Mat4{1.0f}, Vec3{0.92f, 0.05f, 0.92f}),
                                 Vec4{0.4f, 1.0f, 0.45f, 0.5f * pulse});
            for (int k = 0; k < 2; ++k) {
                const f32 a0 = elapsed_ * 7.0f + Pi * static_cast<f32>(k);
                const f32 y = 0.35f + 0.25f * static_cast<f32>(k);
                const Vec3 p0 = feet + Vec3{std::cos(a0) * 0.55f, y, std::sin(a0) * 0.55f};
                const Vec3 p1 = feet + Vec3{std::cos(a0 + 0.9f) * 0.55f, y, std::sin(a0 + 0.9f) * 0.55f};
                renderer_->draw_sprite(p0, p1, 0.05f, Vec4{0.6f, 1.0f, 0.6f, 0.55f}, 0.4f);
            }
        }
    }
    // Power Conduit: an arcane ray from a channelling Cleric to the ally it heals + empowers.
    for (const net::PlayerState& p : snapshot_.players) {
        if (p.link == 0) {
            continue;
        }
        const net::PlayerState* target = nullptr;
        for (const net::PlayerState& q : snapshot_.players) {
            if (q.id == p.link) {
                target = &q;
                break;
            }
        }
        if (target == nullptr) {
            continue;
        }
        const Vec3 a = p.position + Vec3{0.0f, 1.4f, 0.0f};       // the Cleric's staff-head
        const Vec3 b = target->position + Vec3{0.0f, 1.0f, 0.0f}; // the ally's chest
        const f32 len = glm::length(b - a);
        if (len > 0.1f) {
            draw_ray(a, b, Vec3{0.5f, 1.0f, 0.72f}, 0.07f, 0.75f + 0.25f * pulse, static_cast<f32>(p.id));
            for (int i = 0; i < 3; ++i) { // motes flowing along the beam toward the ally
                emit(glm::mix(a, b, frand()), (b - a) * 0.35f, Vec4{0.62f, 1.0f, 0.78f, 0.9f}, 0.3f,
                     0.1f, 1);
            }
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
