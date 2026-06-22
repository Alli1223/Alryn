// ClientApp - particle pool and ability/buff visual effects.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

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

} // namespace alryn::game
