// ClientApp - spell VFX: spell lights, magical beams, rune sigils, the Mage's spells, meteor
// strikes, spell-bolt impacts and the Aegis shield animation. Everything glows through the
// renderer's instanced soft sprites (Renderer::draw_sprite) and lights the world around it.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

namespace alryn::game {

namespace {

// 0..1 hash of an integer - stable per beam / segment / frame (the arc's crackle pattern).
f32 hash01(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return static_cast<f32>(x & 0xFFFFFFu) / static_cast<f32>(0x1000000u);
}

// Two unit axes perpendicular to `dir` (and to each other).
void ortho_basis(const Vec3& dir, Vec3& u, Vec3& v) {
    const Vec3 ref = std::abs(dir.y) < 0.95f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    u = glm::normalize(glm::cross(dir, ref));
    v = glm::cross(dir, u);
}

// The signature colour of each Mage spell (its sigil, beams and light).
Vec3 spell_color(SpellId s) {
    switch (s) {
        case SpellId::Fireball:
        case SpellId::Meteor: return Vec3{1.0f, 0.48f, 0.14f};
        case SpellId::FrostBolt: return Vec3{0.5f, 0.82f, 1.0f};
        case SpellId::Boulder:
        case SpellId::RockWall: return Vec3{0.92f, 0.66f, 0.3f};
        case SpellId::HealBloom: return Vec3{0.45f, 1.0f, 0.5f};
        case SpellId::Empower: return Vec3{1.0f, 0.62f, 0.22f};
        default: return Vec3{0.75f, 0.6f, 1.0f};
    }
}

// The colour of a queued element orb (the Mage's combo weave).
Vec3 element_color(u8 e) {
    switch (static_cast<Element>(e)) {
        case Element::Fire: return Vec3{1.0f, 0.45f, 0.12f};
        case Element::Water: return Vec3{0.45f, 0.8f, 1.0f};
        case Element::Earth: return Vec3{0.9f, 0.64f, 0.28f};
        case Element::Nature: return Vec3{0.45f, 1.0f, 0.45f};
    }
    return Vec3{1.0f};
}

const Vec3 kFireHot{1.0f, 0.86f, 0.45f};  // a fresh ember: yellow-white
const Vec3 kFireCool{0.75f, 0.12f, 0.04f}; // ...cooling to a deep red as it dies

} // namespace

// --- Spell lights ---------------------------------------------------------------------------

void ClientApp::flash_light(const Vec3& pos, const Vec3& color, f32 strength, f32 range, f32 life) {
    if (flash_lights_.size() >= 32) {
        flash_lights_.erase(flash_lights_.begin()); // a busy fight drops its oldest flash
    }
    flash_lights_.push_back({pos, color, strength, range, life, life});
}

void ClientApp::fx_light(const Vec3& pos, const Vec3& color, f32 strength, f32 range) {
    if (strength > 0.02f && range > 0.1f && frame_fx_lights_.size() < 256) {
        frame_fx_lights_.push_back({pos, color * strength, range});
    }
}

void ClientApp::draw_fx_lights() {
    for (const FlashLight& f : flash_lights_) {
        const f32 t = glm::clamp(f.life / f.max_life, 0.0f, 1.0f); // 1 fresh .. 0 spent
        fx_light(f.pos, f.color, f.strength * t * t, f.range * (0.7f + 0.3f * t));
    }
    if (renderer_ == nullptr || frame_fx_lights_.empty()) {
        frame_fx_lights_.clear();
        return;
    }
    // Spell light reads strongest in the dark, but still visibly tints the ground at noon.
    const f32 night = 1.0f - sun_intensity_;
    const f32 scale = 0.32f + 0.3f * night;
    const Vec3 feet = local_feet();
    if (frame_fx_lights_.size() > vfx::max_lights) {
        auto nearer = [&](const FxLight& a, const FxLight& b) {
            return glm::dot(a.pos - feet, a.pos - feet) < glm::dot(b.pos - feet, b.pos - feet);
        };
        std::partial_sort(frame_fx_lights_.begin(),
                          frame_fx_lights_.begin() + static_cast<std::ptrdiff_t>(vfx::max_lights),
                          frame_fx_lights_.end(), nearer);
        frame_fx_lights_.resize(vfx::max_lights);
    }
    for (const FxLight& l : frame_fx_lights_) {
        Renderer::SpotLight sl;
        sl.position = l.pos;
        sl.direction = Vec3{0.0f, -1.0f, 0.0f};
        sl.color = l.color * scale;
        sl.range = l.range;
        sl.cone_outer_cos = -1.0f; // omni: the whole sphere (clamped to -0.999 by the renderer)
        sl.cone_inner_cos = -0.98f;
        sl.cast_shadow = false; // the cheap unshadowed pool
        sl.priority = true;     // ahead of the lanterns (capped above, so they keep most slots)
        renderer_->add_light(sl);
    }
    frame_fx_lights_.clear();
}

// --- Beams + sigils ------------------------------------------------------------------------

void ClientApp::beam(const Vec3& a, const Vec3& b, const Vec4& color, f32 width, f32 life, u8 style,
                     f32 grow) {
    if (beams_.size() >= 48) {
        beams_.erase(beams_.begin());
    }
    Beam bm;
    bm.a = a;
    bm.b = b;
    bm.color = color;
    bm.width = width;
    bm.life = life;
    bm.max_life = life;
    bm.grow = grow;
    bm.style = style;
    bm.seed = static_cast<u32>(frand() * 16777215.0f);
    beams_.push_back(bm);
}

void ClientApp::glyph(const Vec3& center, f32 radius, const Vec4& color, f32 life, int points) {
    if (glyphs_.size() >= 24) {
        glyphs_.erase(glyphs_.begin());
    }
    glyphs_.push_back({center, radius, color, life, life, frand(0.0f, TwoPi), points});
}

void ClientApp::sprite_circle(const Vec3& c, const Vec3& u, const Vec3& v, f32 radius, int segs,
                              f32 width, const Vec4& color, f32 shimmer) {
    if (renderer_ == nullptr || segs < 3 || color.a <= 0.01f) {
        return;
    }
    Vec3 prev = c + u * radius;
    for (int i = 1; i <= segs; ++i) {
        const f32 a = TwoPi * static_cast<f32>(i) / static_cast<f32>(segs);
        const Vec3 p = c + (u * std::cos(a) + v * std::sin(a)) * radius;
        const f32 band = 0.5f + 0.5f * std::sin(a * 3.0f - elapsed_ * 2.6f); // bands run round it
        const f32 k = 1.0f - shimmer + shimmer * band * band;
        renderer_->draw_sprite(prev, p, width, Vec4{Vec3{color}, color.a * k}, 0.35f);
        prev = p;
    }
}

void ClientApp::sphere_rim(const Vec3& c, f32 radius, const Vec3& color, f32 intensity, int segs,
                           f32 width) {
    Vec3 to_eye = camera_.position() - c;
    const f32 D = glm::length(to_eye);
    if (D <= radius * 1.05f) {
        return; // the camera is inside it - no silhouette to trace
    }
    to_eye /= D;
    const Vec3 u = glm::normalize(glm::cross(to_eye, Vec3{0.0f, 1.0f, 0.0f}));
    const Vec3 v = glm::cross(u, to_eye);
    // In perspective the silhouette is a slightly smaller circle, a little nearer the eye.
    const f32 rr = radius * std::sqrt(1.0f - (radius / D) * (radius / D));
    const Vec3 rc = c + to_eye * (radius * radius / D);
    sprite_circle(rc, u, v, rr, segs, width, Vec4{color, 0.5f * intensity}, 0.55f);
    sprite_circle(rc, u, v, rr * 0.96f, segs, width * 3.5f, Vec4{color, 0.07f * intensity}); // inner sheen
}

void ClientApp::draw_glyph(const Vec3& center, f32 radius, const Vec3& color, f32 intensity,
                           int points, f32 spin) {
    if (renderer_ == nullptr || intensity <= 0.01f || radius <= 0.05f) {
        return;
    }
    const f32 w = glm::clamp(radius * 0.035f, 0.04f, 0.14f); // line width scales with the sigil
    // Lifted off the ground a touch so the sprites' soft depth fade keeps the lines bright.
    const Vec3 c = center + Vec3{0.0f, w * 1.3f + 0.04f, 0.0f};
    const Vec3 u{std::cos(spin), 0.0f, std::sin(spin)};
    const Vec3 v{-std::sin(spin), 0.0f, std::cos(spin)};
    const int segs = std::clamp(static_cast<int>(radius * 14.0f), 20, 72);
    sprite_circle(c, u, v, radius, segs, w, Vec4{color, 0.75f * intensity}, 0.35f);
    sprite_circle(c, v, u, radius * 0.78f, segs, w * 0.7f, Vec4{color, 0.5f * intensity}, 0.5f);
    // The star: each vertex joined to the one `step` round (a pentagram / hexagram / octagram).
    const int n = std::max(points, 3);
    const int step = n >= 7 ? 3 : 2;
    auto vert = [&](int i) {
        const f32 a = TwoPi * static_cast<f32>(i) / static_cast<f32>(n);
        return c + (u * std::cos(a) + v * std::sin(a)) * (radius * 0.78f);
    };
    const Vec4 star{color, 0.45f * intensity};
    for (int i = 0; i < n; ++i) {
        renderer_->draw_sprite(vert(i), vert((i + step) % n), w * 0.6f, star, 0.3f);
    }
    // Rune marks orbiting between the rings, against the sigil's spin.
    const int marks = n * 2;
    for (int i = 0; i < marks; ++i) {
        const f32 a = TwoPi * static_cast<f32>(i) / static_cast<f32>(marks) - spin * 2.0f;
        const Vec3 p = c + Vec3{std::cos(a), 0.0f, std::sin(a)} * (radius * 0.89f);
        renderer_->draw_sprite(p, w * 1.6f, Vec4{color, 0.6f * intensity}, 0.8f);
    }
}

void ClientApp::draw_ray(const Vec3& a, const Vec3& b, const Vec3& color, f32 width, f32 intensity,
                         f32 phase) {
    const Vec3 axis = b - a;
    const f32 len = glm::length(axis);
    if (renderer_ == nullptr || len < 0.05f || intensity <= 0.01f) {
        return;
    }
    const f32 flick = 0.88f + 0.12f * std::sin(elapsed_ * 37.0f + phase * 5.0f);
    renderer_->draw_sprite(a, b, width * 3.4f, Vec4{color, 0.2f * intensity}, 0.0f);   // soft halo
    renderer_->draw_sprite(a, b, width, Vec4{color, 0.95f * intensity * flick}, 1.0f); // hot core
    // Twin strands spiralling round the ray, tapering in at both ends.
    Vec3 u, v;
    ortho_basis(axis / len, u, v);
    const int segs = std::clamp(static_cast<int>(len * 3.0f), 6, 48);
    const Vec4 strand{glm::mix(color, Vec3{1.0f}, 0.25f), 0.55f * intensity};
    for (int s = 0; s < 2; ++s) {
        Vec3 prev = a;
        for (int i = 1; i <= segs; ++i) {
            const f32 f = static_cast<f32>(i) / static_cast<f32>(segs);
            const f32 ang = f * len * 2.4f - elapsed_ * 13.0f + phase + Pi * static_cast<f32>(s);
            const f32 taper =
                std::min(1.0f, f * len / 0.8f) * std::min(1.0f, (1.0f - f) * len / 0.8f);
            const Vec3 p = a + axis * f + (u * std::cos(ang) + v * std::sin(ang)) * (width * 2.4f * taper);
            renderer_->draw_sprite(prev, p, width * 0.42f, strand, 0.5f);
            prev = p;
        }
    }
    renderer_->draw_sprite(a, width * 3.2f, Vec4{color, 0.7f * intensity}, 0.9f); // muzzle flare
    renderer_->draw_sprite(b, width * 4.2f, Vec4{color, 0.8f * intensity}, 1.0f); // impact flare
    fx_light(a + axis * 0.5f, color, 2.0f * intensity, std::min(len * 0.6f + 3.0f, 10.0f));
    fx_light(b, color, 2.2f * intensity, 6.0f);
}

void ClientApp::draw_arc(const Vec3& a, const Vec3& b, const Vec3& color, f32 width, f32 intensity,
                         u32 seed) {
    const Vec3 axis = b - a;
    const f32 len = glm::length(axis);
    if (renderer_ == nullptr || len < 0.05f || intensity <= 0.01f) {
        return;
    }
    Vec3 u, v;
    ortho_basis(axis / len, u, v);
    const u32 frame = static_cast<u32>(elapsed_ * 24.0f); // re-jitters ~24 times a second
    const int segs = std::clamp(static_cast<int>(len * 2.2f), 4, 28);
    const f32 amp = std::min(0.12f + len * 0.06f, 0.7f);
    const Vec3 hot = glm::mix(color, Vec3{1.0f}, 0.45f);
    renderer_->draw_sprite(a, b, width * 4.0f, Vec4{color, 0.14f * intensity}, 0.0f); // ionised glow
    for (u32 fork = 0; fork < 2; ++fork) { // the main bolt + a fainter, wilder fork
        const f32 wild = fork == 0 ? 1.0f : 1.6f;
        const Vec4 col{fork == 0 ? hot : color, (fork == 0 ? 1.0f : 0.5f) * intensity};
        Vec3 prev = a;
        for (int i = 1; i <= segs; ++i) {
            const f32 f = static_cast<f32>(i) / static_cast<f32>(segs);
            const u32 h = seed * 7919u + frame * 104729u + static_cast<u32>(i) * 31u + fork * 977u;
            const Vec3 off = (u * (hash01(h) * 2.0f - 1.0f) + v * (hash01(h ^ 0x9e3779b9u) * 2.0f - 1.0f)) *
                             (amp * wild * std::sin(f * Pi)); // pinned at both ends
            const Vec3 p = i == segs ? b : a + axis * f + off;
            renderer_->draw_sprite(prev, p, width * (fork == 0 ? 0.55f : 0.35f), col, 0.8f);
            prev = p;
        }
    }
    renderer_->draw_sprite(a, width * 3.0f, Vec4{color, 0.6f * intensity}, 0.9f);
    renderer_->draw_sprite(b, width * 4.0f, Vec4{color, 0.8f * intensity}, 1.0f);
    fx_light(a + axis * 0.5f, color, 1.8f * intensity, std::min(len * 0.6f + 3.0f, 10.0f));
    fx_light(b, color, 2.0f * intensity, 6.0f);
}

void ClientApp::draw_spell_fx() {
    if (renderer_ == nullptr) {
        return;
    }
    for (const Beam& bm : beams_) {
        const f32 t = glm::clamp(bm.life / bm.max_life, 0.0f, 1.0f); // 1 fresh .. 0 spent
        const f32 env = std::min(1.0f, t / 0.45f);                    // full, then fades out
        const f32 flash = glm::smoothstep(0.7f, 1.0f, t);             // fat as it first fires
        const f32 w = bm.width * (0.6f + 0.4f * env) * (1.0f + 0.7f * flash);
        Vec3 b = bm.b;
        if (bm.grow > 0.0f) { // the head races out from `a`
            const f32 len = glm::length(bm.b - bm.a);
            const f32 reach = bm.age * bm.grow;
            if (reach < len) {
                b = bm.a + (bm.b - bm.a) * (reach / std::max(len, 1e-3f));
            }
        }
        const Vec3 rgb{bm.color};
        const f32 k = bm.color.a * env;
        if (bm.style == 1) {
            draw_arc(bm.a, b, rgb, w, k, bm.seed);
        } else if (bm.style == 2) { // a pillar of light striking down from `a` (the sky) onto `b`
            renderer_->draw_sprite(bm.a, b, w * 3.0f, Vec4{rgb, 0.22f * k}, 0.0f);
            renderer_->draw_sprite(bm.a, b, w, Vec4{rgb, 0.95f * k}, 1.0f);
            for (int i = 0; i < 3; ++i) { // rings of light sliding down the column
                const f32 s = std::fmod(bm.age * 1.6f + static_cast<f32>(i) / 3.0f, 1.0f);
                sprite_circle(glm::mix(bm.a, b, s), Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f},
                              w * (2.0f + 2.5f * s), 14, w * 0.3f, Vec4{rgb, 0.5f * k * (1.0f - 0.5f * s)});
            }
            renderer_->draw_sprite(b + Vec3{0.0f, w * 2.5f, 0.0f}, w * 6.0f, Vec4{rgb, 0.6f * k}, 0.9f);
            fx_light(b + Vec3{0.0f, 1.0f, 0.0f}, rgb, 3.0f * k, 9.0f);
        } else {
            draw_ray(bm.a, b, rgb, w, k, static_cast<f32>(bm.seed % 1000u) * 0.01f);
        }
    }

    for (const Glyph& g : glyphs_) {
        const f32 age = g.max_life - g.life;
        const f32 t = glm::clamp(g.life / g.max_life, 0.0f, 1.0f);
        const f32 open = glm::smoothstep(0.0f, 0.18f, age); // snaps open...
        const f32 fade = std::min(1.0f, t / 0.45f);         // ...then fades over its last stretch
        const f32 k = g.color.a * fade * (1.0f + 0.6f * (1.0f - open));
        draw_glyph(g.center, g.radius * (0.55f + 0.45f * open), Vec3{g.color}, k, g.points,
                   g.spin + age * 1.4f);
        fx_light(g.center + Vec3{0.0f, 0.6f, 0.0f}, Vec3{g.color}, 0.9f * k, g.radius * 1.6f + 2.0f);
    }

    for (const MeteorFx& m : meteors_) {
        const f32 k = glm::clamp(m.t / m.dur, 0.0f, 1.0f);
        const Vec3 p = glm::mix(m.from, m.to, k * k); // accelerating as it falls
        const Vec3 back = glm::normalize(m.from - m.to);
        renderer_->draw_sprite(p, p + back * (2.5f + 2.0f * k), 0.7f, Vec4{1.0f, 0.5f, 0.15f, 0.9f}, 0.3f);
        renderer_->draw_sprite(p, 0.75f, Vec4{1.0f, 0.78f, 0.4f, 1.6f}, 1.0f); // white-hot head
        renderer_->draw_sprite(p, 1.8f, Vec4{1.0f, 0.4f, 0.1f, 0.4f}, 0.0f);  // heat glow
        fx_light(p, Vec3{1.0f, 0.55f, 0.2f}, 3.5f, 14.0f);
    }

    // The local Mage weaving a combo: each queued element orbits the caster as a glowing orb.
    if (role_ == PlayerRole::Mage && casting_) {
        const Vec3 core = local_feet() + Vec3{0.0f, 1.45f, 0.0f};
        if (combo_n_ == 0) { // a faint gathering glow while the weave is still empty
            renderer_->draw_sprite(core, 0.3f,
                                   Vec4{0.75f, 0.6f, 1.0f, 0.35f + 0.15f * std::sin(elapsed_ * 6.0f)}, 0.6f);
        }
        for (u8 i = 0; i < combo_n_; ++i) {
            const f32 a = elapsed_ * 3.2f + TwoPi * static_cast<f32>(i) / static_cast<f32>(combo_n_);
            const Vec3 p = core + Vec3{std::cos(a) * 0.8f, 0.2f * std::sin(a * 2.0f), std::sin(a) * 0.8f};
            const Vec3 col = element_color(combo_[i]);
            renderer_->draw_sprite(p, 0.2f, Vec4{col, 1.3f}, 1.0f);
            renderer_->draw_sprite(p, 0.55f, Vec4{col, 0.3f}, 0.0f);
            emit(p, rand_dir() * 0.25f, Vec4{col, 0.7f}, 0.35f, 0.1f, 1); // a short sparkling wake
            fx_light(p, col, 1.0f, 4.5f);
        }
    }
}

// --- Spell impacts --------------------------------------------------------------------------

void ClientApp::meteor_impact(const Vec3& at) {
    const Vec3 g = at + Vec3{0.0f, 0.2f, 0.0f};
    emit(g + Vec3{0.0f, 0.8f, 0.0f}, Vec3{0.0f}, Vec4{1.0f, 0.8f, 0.45f, 1.0f}, 0.3f, 3.6f, 1); // flash
    emit_ring(at, Vec4{1.0f, 0.55f, 0.18f, 0.95f}, 44, kMeteorRadius * 2.2f, 0.55f, 0.28f); // shockwave
    emit_ring(at, Vec4{1.0f, 0.82f, 0.5f, 0.8f}, 28, kMeteorRadius * 1.1f, 0.8f, 0.2f);
    for (int i = 0; i < 70; ++i) { // a fireball of embers erupting up and out
        Vec3 d = rand_dir();
        d.y = std::abs(d.y) * 0.9f + 0.2f;
        emit_ember(g, d * frand(2.5f, 9.0f), kFireHot, kFireCool, frand(0.5f, 1.1f), frand(0.18f, 0.42f),
                   frand(-2.0f, 3.0f));
    }
    for (int i = 0; i < 24; ++i) { // white-hot sparks arcing out and falling
        Vec3 d = rand_dir();
        d.y = std::abs(d.y) + 0.3f;
        emit(g, d * frand(8.0f, 14.0f), Vec4{1.0f, 0.9f, 0.6f, 1.0f}, frand(0.4f, 0.8f), 0.08f, 1, 14.0f,
             0.6f);
    }
    for (int k = 0; k < 10; ++k) { // flame tongues licking up around the crater
        const f32 a = TwoPi * static_cast<f32>(k) / 10.0f + frand(-0.2f, 0.2f);
        const Vec3 base = at + Vec3{std::cos(a), 0.0f, std::sin(a)} * (kMeteorRadius * frand(0.4f, 0.85f)) +
                          Vec3{0.0f, 0.15f, 0.0f};
        for (int j = 0; j < 4; ++j) {
            emit_ember(base, Vec3{frand(-0.4f, 0.4f), frand(3.0f, 6.0f), frand(-0.4f, 0.4f)}, kFireHot,
                       kFireCool, frand(0.5f, 0.8f), 0.3f, -1.5f);
        }
    }
    for (int i = 0; i < 12; ++i) { // molten rock flung clear
        Vec3 d = rand_dir();
        d.y = std::abs(d.y) + 0.6f;
        emit(g, d * frand(4.0f, 8.0f), Vec4{0.35f, 0.2f, 0.12f, 1.0f}, frand(0.7f, 1.1f),
             frand(0.12f, 0.22f), 0, 16.0f, 0.3f);
    }
    for (int i = 0; i < 30; ++i) { // embers smouldering in the crater a while after
        const f32 a = frand(0.0f, TwoPi);
        const f32 r = std::sqrt(frand()) * kMeteorRadius;
        emit_ember(at + Vec3{std::cos(a) * r, 0.2f, std::sin(a) * r}, Vec3{0.0f, frand(0.2f, 0.8f), 0.0f},
                   Vec3{1.0f, 0.6f, 0.2f}, Vec3{0.5f, 0.08f, 0.02f}, frand(1.5f, 3.0f), frand(0.12f, 0.22f),
                   -0.2f, 0.8f);
    }
    glyph(at, kMeteorRadius, Vec4{1.0f, 0.4f, 0.12f, 0.9f}, 1.6f, 8); // the scorched sigil burns out
    flash_light(at + Vec3{0.0f, 1.5f, 0.0f}, Vec3{1.0f, 0.5f, 0.18f}, 7.0f, 18.0f, 0.9f);
    flash_light(at + Vec3{0.0f, 0.8f, 0.0f}, Vec3{1.0f, 0.38f, 0.1f}, 2.0f, 9.0f, 3.0f); // crater glow
}

void ClientApp::projectile_impact(u8 kind, const Vec3& at) {
    const Vec3 up{0.0f, 0.4f, 0.0f};
    switch (kind) {
        case 5: // Fireball: a fiery blossom of embers + sparks
            emit(at, Vec3{0.0f}, Vec4{1.0f, 0.78f, 0.4f, 1.0f}, 0.2f, 1.9f, 1);
            for (int i = 0; i < 38; ++i) {
                emit_ember(at, rand_dir() * frand(2.0f, 6.5f) + Vec3{0.0f, 1.2f, 0.0f}, kFireHot, kFireCool,
                           frand(0.4f, 0.8f), frand(0.15f, 0.32f), frand(-2.0f, 2.0f));
            }
            for (int i = 0; i < 12; ++i) {
                Vec3 d = rand_dir();
                d.y = std::abs(d.y) + 0.2f;
                emit(at, d * frand(6.0f, 11.0f), Vec4{1.0f, 0.85f, 0.5f, 1.0f}, frand(0.3f, 0.6f), 0.07f, 1,
                     12.0f, 0.6f);
            }
            emit_ring(at, Vec4{1.0f, 0.55f, 0.2f, 0.9f}, 22, kFireballRadius * 2.4f, 0.4f, 0.18f);
            flash_light(at + up, Vec3{1.0f, 0.55f, 0.2f}, 5.0f, 11.0f, 0.5f);
            break;
        case 6: // Frost Bolt: a burst of ice shards in a cold mist
            emit(at, Vec3{0.0f}, Vec4{0.8f, 0.95f, 1.0f, 1.0f}, 0.18f, 1.4f, 1);
            for (int i = 0; i < 20; ++i) {
                Vec3 d = rand_dir();
                d.y = std::abs(d.y) * 0.8f + 0.1f;
                emit(at, d * frand(4.0f, 9.0f), Vec4{0.8f, 0.94f, 1.0f, 1.0f}, frand(0.35f, 0.6f), 0.07f, 1,
                     11.0f, 0.5f);
            }
            for (int i = 0; i < 18; ++i) {
                emit(at + rand_dir() * 0.3f, rand_dir() * frand(0.4f, 1.4f) + Vec3{0.0f, 0.3f, 0.0f},
                     Vec4{0.55f, 0.82f, 1.0f, 0.6f}, frand(0.7f, 1.2f), frand(0.2f, 0.35f), 1, -0.2f, 2.5f);
            }
            emit_ring(at, Vec4{0.6f, 0.88f, 1.0f, 0.85f}, 20, 3.2f, 0.45f, 0.14f);
            flash_light(at + up, Vec3{0.5f, 0.8f, 1.0f}, 3.8f, 9.0f, 0.45f);
            break;
        case 7: // Boulder: dust, flung chips and a faint amber pulse of earth magic
            emit_burst(at, Vec4{0.5f, 0.44f, 0.36f, 0.75f}, 14, 3.0f, 0.7f, 0.24f, 0, 1.2f, 2.0f);
            for (int i = 0; i < 12; ++i) {
                Vec3 d = rand_dir();
                d.y = std::abs(d.y) + 0.5f;
                emit(at, d * frand(3.0f, 6.5f), Vec4{0.42f, 0.38f, 0.34f, 1.0f}, frand(0.6f, 0.9f),
                     frand(0.08f, 0.16f), 0, 15.0f, 0.3f);
            }
            emit_ring(at, Vec4{0.95f, 0.72f, 0.38f, 0.55f}, 18, 3.0f, 0.4f, 0.16f);
            flash_light(at, Vec3{1.0f, 0.7f, 0.35f}, 1.6f, 6.0f, 0.3f);
            break;
        case 2: // holy bolt: a radiant starburst with golden motes drifting up
            emit(at, Vec3{0.0f}, Vec4{0.9f, 1.0f, 0.88f, 1.0f}, 0.2f, 1.5f, 1);
            for (int i = 0; i < 18; ++i) {
                emit(at, rand_dir() * frand(2.0f, 6.0f), Vec4{0.7f, 1.0f, 0.82f, 0.95f}, frand(0.35f, 0.6f),
                     frand(0.08f, 0.14f), 1, 0.0f, 3.0f);
            }
            for (int i = 0; i < 10; ++i) {
                emit(at + rand_dir() * 0.4f, Vec3{0.0f, frand(1.0f, 2.5f), 0.0f},
                     Vec4{1.0f, 0.92f, 0.6f, 0.9f}, frand(0.6f, 1.0f), 0.08f, 1, -0.5f);
            }
            emit_ring(at, Vec4{0.8f, 1.0f, 0.88f, 0.85f}, 18, 3.2f, 0.4f, 0.14f);
            flash_light(at + up, Vec3{0.7f, 1.0f, 0.8f}, 3.8f, 9.0f, 0.45f);
            break;
        case 4: // the Cleric's arcane bolt: a violet pop
            emit(at, Vec3{0.0f}, Vec4{0.85f, 0.65f, 1.0f, 1.0f}, 0.16f, 1.1f, 1);
            for (int i = 0; i < 16; ++i) {
                emit(at, rand_dir() * frand(2.0f, 5.5f), Vec4{0.75f, 0.5f, 1.0f, 0.95f}, frand(0.3f, 0.55f),
                     frand(0.07f, 0.12f), 1, 0.0f, 3.0f);
            }
            flash_light(at, Vec3{0.65f, 0.45f, 1.0f}, 2.8f, 7.0f, 0.35f);
            break;
        default: break;
    }
}

void ClientApp::track_projectiles() {
    if (!have_snapshot_ || snapshot_.tick == proj_track_tick_) {
        return; // only re-match when a new snapshot has landed
    }
    // Seconds since the last matched snapshot (the server ticks at ~60 Hz), so the search reach
    // still covers a bolt's travel if a frame swallowed a snapshot or two.
    const f32 dt = proj_track_tick_ == 0
                       ? 1.0f / 60.0f
                       : glm::clamp(static_cast<f32>(snapshot_.tick - proj_track_tick_) / 60.0f,
                                    1.0f / 60.0f, 0.5f);
    proj_track_tick_ = snapshot_.tick;
    auto spell_bolt = [](u8 kind) { return kind == 2 || kind == 4 || kind == 5 || kind == 6 || kind == 7; };
    for (ProjectileTrack& t : proj_tracks_) {
        t.seen = false;
    }
    for (const net::ProjectileState& pr : snapshot_.projectiles) {
        if (!spell_bolt(pr.kind)) {
            continue;
        }
        ProjectileTrack* best = nullptr;
        f32 best_d = 0.9f + 40.0f * dt;
        for (ProjectileTrack& t : proj_tracks_) {
            if (t.seen || t.kind != pr.kind) {
                continue;
            }
            const Vec3 guess = t.resting ? t.pos : t.pos + t.dir * (30.0f * dt);
            const f32 d = glm::length(pr.position - guess);
            if (d < best_d) {
                best_d = d;
                best = &t;
            }
        }
        if (best == nullptr) {
            proj_tracks_.push_back({pr.position, pr.dir, pr.kind, true, false}); // a fresh bolt
            continue;
        }
        if (!best->resting && glm::length(pr.position - best->pos) < 0.02f) {
            best->resting = true; // it struck the ground (or a wall) and stopped dead: burst there
            projectile_impact(pr.kind, pr.position);
        }
        best->pos = pr.position;
        best->dir = pr.dir;
        best->seen = true;
    }
    for (const ProjectileTrack& t : proj_tracks_) {
        if (!t.seen && !t.resting) {
            projectile_impact(t.kind, t.pos + t.dir * 0.25f); // gone mid-flight: it hit something
        }
    }
    std::erase_if(proj_tracks_, [](const ProjectileTrack& t) { return !t.seen; });
}

bool ClientApp::projectile_spent(const net::ProjectileState& pr) const {
    for (const ProjectileTrack& t : proj_tracks_) {
        if (t.resting && t.kind == pr.kind && glm::length(t.pos - pr.position) < 0.05f) {
            return true;
        }
    }
    return false;
}

// --- Aegis shields ----------------------------------------------------------------------------

void ClientApp::update_shields(Timestep dt) {
    if (!have_snapshot_) {
        return;
    }
    ++shield_stamp_;
    const f32 R = vfx::shield_radius;
    const Vec3 ward{0.55f, 0.85f, 1.0f};
    auto step = [&](u64 key, const Vec3& feet, u8 s) {
        ShieldFx& fx = shield_fx_[key];
        fx.stamp = shield_stamp_;
        const Vec3 c = feet + Vec3{0.0f, 0.95f, 0.0f};
        if (s > 0 && fx.last == 0) { // the ward snaps up: a flash, a rising ring, motes off the shell
            fx.age = 0.0f;
            fx.hit = 0.0f;
            emit(c, Vec3{0.0f}, Vec4{0.7f, 0.9f, 1.0f, 1.0f}, 0.22f, 2.2f, 1);
            emit_ring(feet, Vec4{ward, 0.9f}, 30, 4.0f, 0.55f, 0.18f);
            for (int i = 0; i < 26; ++i) {
                const Vec3 d = rand_dir();
                emit(c + d * R, d * frand(0.5f, 1.5f), Vec4{0.6f, 0.88f, 1.0f, 0.9f}, frand(0.4f, 0.7f), 0.09f,
                     1, 0.0f, 2.0f);
            }
            flash_light(c, Vec3{0.5f, 0.8f, 1.0f}, 4.0f, 9.0f, 0.5f);
        } else if (s > 0 && static_cast<int>(s) + 6 < static_cast<int>(fx.last)) {
            // It soaked a blow: the shell flashes and sparks fly off where it was struck.
            fx.hit = 1.0f;
            const Vec3 d = rand_dir();
            for (int i = 0; i < 10; ++i) {
                emit(c + d * R, (d + rand_dir() * 0.6f) * frand(2.0f, 5.0f), Vec4{0.75f, 0.92f, 1.0f, 1.0f},
                     frand(0.25f, 0.45f), 0.07f, 1, 6.0f, 1.0f);
            }
            flash_light(c, Vec3{0.5f, 0.8f, 1.0f}, 2.5f, 7.0f, 0.25f);
        } else if (s == 0 && fx.last > 0) { // spent: the bubble shatters into falling shards
            for (int i = 0; i < 36; ++i) {
                const Vec3 d = rand_dir();
                emit(c + d * R * frand(0.8f, 1.0f), d * frand(1.5f, 4.5f) + Vec3{0.0f, 1.0f, 0.0f},
                     Vec4{0.65f, 0.88f, 1.0f, 0.95f}, frand(0.5f, 0.9f), frand(0.06f, 0.11f), 1, 9.0f, 0.8f);
            }
            flash_light(c, Vec3{0.5f, 0.8f, 1.0f}, 3.0f, 8.0f, 0.35f);
        }
        fx.age += dt.seconds;
        fx.hit = std::max(0.0f, fx.hit - dt.seconds * 3.5f);
        fx.last = s;
    };
    for (const net::PlayerState& p : snapshot_.players) {
        step(static_cast<u64>(p.id), p.position, p.shield);
    }
    for (const net::VillagerState& v : snapshot_.villagers) {
        step((u64{1} << 32) | static_cast<u64>(v.id), v.position, v.shield);
    }
    std::erase_if(shield_fx_, [&](const auto& kv) { return kv.second.stamp != shield_stamp_; });
}

// --- The Mage's spells ------------------------------------------------------------------------

void ClientApp::spawn_spell_vfx(SpellId spell, const Vec3& feet, f32 yaw, const Vec3& aim) {
    if (spell == SpellId::None) {
        return;
    }
    const Vec3 facing{std::cos(yaw), 0.0f, std::sin(yaw)};
    const Vec3 staff = feet + Vec3{0.0f, 1.45f, 0.0f} + facing * 0.45f; // the raised staff-head
    const Vec3 col = spell_color(spell);
    const Vec3 to_aim = aim - staff;
    const Vec3 dir = glm::length(to_aim) > 0.3f ? glm::normalize(to_aim) : facing;

    // Every spell opens a sigil under the caster and gathers light into the staff.
    glyph(feet, 1.25f, Vec4{col, 1.0f}, 0.9f, spell == SpellId::HealBloom ? 5 : 6);
    emit(staff, Vec3{0.0f}, Vec4{glm::mix(col, Vec3{1.0f}, 0.3f), 1.0f}, 0.22f, 0.8f, 1);
    for (int i = 0; i < 12; ++i) {
        const Vec3 from = staff + rand_dir() * frand(0.6f, 1.1f);
        emit(from, (staff - from) * 4.0f, Vec4{col, 0.9f}, 0.25f, 0.09f, 1, 0.0f, 0.0f);
    }
    flash_light(staff, col, 3.0f, 8.0f, 0.35f);

    // Every other player within `r` of the caster (the allies a group spell links to).
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

    switch (spell) {
        case SpellId::Fireball: // a gout of flame from the staff (the bolt carries the rest)
            for (int i = 0; i < 14; ++i) {
                emit_ember(staff, dir * frand(3.0f, 8.0f) + rand_dir() * 1.2f, kFireHot, kFireCool,
                           frand(0.25f, 0.45f), frand(0.12f, 0.24f), 0.0f);
            }
            break;
        case SpellId::FrostBolt: // FROST LANCE: an icy ray races out ahead of the bolt
            beam(staff, aim + Vec3{0.0f, 0.9f, 0.0f}, Vec4{0.4f, 0.7f, 1.0f, 1.0f}, 0.08f, 0.5f, 0, 34.0f);
            for (int i = 0; i < 12; ++i) {
                emit(staff, dir * frand(2.0f, 6.0f) + rand_dir() * 1.0f, Vec4{0.8f, 0.95f, 1.0f, 0.9f},
                     frand(0.3f, 0.5f), 0.07f, 1, 0.0f, 2.0f);
            }
            break;
        case SpellId::Boulder: // the earth heaves: a dust ring and stones lifting off the ground
            emit_ring(feet, Vec4{0.95f, 0.72f, 0.4f, 0.6f}, 20, 3.0f, 0.45f, 0.16f);
            for (int i = 0; i < 10; ++i) {
                emit(feet + Vec3{frand(-0.8f, 0.8f), 0.1f, frand(-0.8f, 0.8f)},
                     Vec3{frand(-0.5f, 0.5f), frand(2.5f, 4.5f), frand(-0.5f, 0.5f)},
                     Vec4{0.45f, 0.4f, 0.34f, 1.0f}, frand(0.5f, 0.8f), frand(0.08f, 0.14f), 0, 9.0f, 0.5f);
            }
            break;
        case SpellId::Meteor: {
            // The Mage calls it down: a ray thrown into the sky, a warning sigil + a targeting
            // column on the spot, then the meteor streaks in (meteor_impact bursts it on arrival).
            beam(staff, staff + Vec3{0.0f, 16.0f, 0.0f}, Vec4{1.0f, 0.5f, 0.15f, 1.0f}, 0.1f, 0.35f, 0, 60.0f);
            glyph(aim, kMeteorRadius, Vec4{1.0f, 0.35f, 0.1f, 0.9f}, 0.7f, 8);
            beam(aim + Vec3{0.0f, 20.0f, 0.0f}, aim, Vec4{1.0f, 0.45f, 0.12f, 0.6f}, 0.12f, 0.5f, 2);
            if (meteors_.size() < 8) {
                meteors_.push_back({aim + Vec3{0.0f, 22.0f, 0.0f} - facing * 7.0f, aim, 0.0f, 0.45f});
            }
            break;
        }
        case SpellId::HealBloom: { // a nature vortex blooming outward + healing links to allies
            for (int i = 0; i < 44; ++i) {
                const f32 a = frand(0.0f, TwoPi);
                const f32 r = frand(0.3f, 1.4f);
                const Vec3 swirl = Vec3{-std::sin(a), 0.0f, std::cos(a)} * frand(1.5f, 3.0f);
                emit(feet + Vec3{std::cos(a) * r, frand(0.1f, 0.6f), std::sin(a) * r},
                     swirl + Vec3{0.0f, frand(1.6f, 3.6f), 0.0f},
                     i % 4 == 0 ? Vec4{1.0f, 0.9f, 0.5f, 0.9f} : Vec4{0.5f, 1.0f, 0.55f, 0.9f},
                     frand(0.8f, 1.3f), frand(0.09f, 0.16f), 1, -0.6f, 0.8f);
            }
            emit_ring(feet, Vec4{0.5f, 1.0f, 0.6f, 0.9f}, 48, kHealBloomRadius * 1.1f, 0.8f, 0.2f);
            beam(feet + Vec3{0.0f, 9.0f, 0.0f}, feet, Vec4{0.55f, 1.0f, 0.6f, 0.8f}, 0.3f, 0.8f, 2);
            for (int i = 0; i < 26; ++i) { // petals drifting down across the bloom
                const f32 a = frand(0.0f, TwoPi);
                const f32 r = std::sqrt(frand()) * kHealBloomRadius;
                emit(feet + Vec3{std::cos(a) * r, frand(1.0f, 2.5f), std::sin(a) * r},
                     Vec3{frand(-0.4f, 0.4f), frand(-0.3f, 0.2f), frand(-0.4f, 0.4f)},
                     i % 3 == 0 ? Vec4{1.0f, 0.75f, 0.85f, 0.8f} : Vec4{0.6f, 1.0f, 0.6f, 0.8f},
                     frand(1.2f, 2.0f), 0.1f, 1, 0.3f, 0.5f);
            }
            for_allies(kHealBloomRadius, [&](const net::PlayerState& p) {
                beam(staff, p.position + Vec3{0.0f, 1.0f, 0.0f}, Vec4{0.55f, 1.0f, 0.6f, 0.9f}, 0.06f, 0.55f,
                     0, 40.0f);
                emit_burst(p.position + Vec3{0.0f, 0.9f, 0.0f}, Vec4{0.6f, 1.0f, 0.65f, 0.9f}, 14, 2.5f, 0.6f,
                           0.12f, 1, 1.5f);
            });
            flash_light(feet + Vec3{0.0f, 1.5f, 0.0f}, Vec3{0.45f, 1.0f, 0.5f}, 4.0f, 12.0f, 1.1f);
            break;
        }
        case SpellId::Empower: { // RUNE OF VIGOUR: a blazing sigil + crackling links to every ally
            glyph(feet, 2.4f, Vec4{1.0f, 0.6f, 0.2f, 1.0f}, 1.2f, 8);
            for (int i = 0; i < 30; ++i) {
                const f32 a = TwoPi * static_cast<f32>(i) / 30.0f;
                emit_ember(feet + Vec3{std::cos(a) * 1.2f, 0.2f, std::sin(a) * 1.2f},
                           Vec3{-std::sin(a) * 2.0f, frand(2.0f, 4.0f), std::cos(a) * 2.0f},
                           Vec3{1.0f, 0.8f, 0.4f}, Vec3{0.9f, 0.25f, 0.05f}, frand(0.6f, 0.9f), 0.16f, -0.5f);
            }
            for_allies(kHealBloomRadius, [&](const net::PlayerState& p) {
                beam(staff, p.position + Vec3{0.0f, 1.0f, 0.0f}, Vec4{1.0f, 0.62f, 0.22f, 1.0f}, 0.07f, 0.6f, 1);
                emit_burst(p.position + Vec3{0.0f, 0.9f, 0.0f}, Vec4{1.0f, 0.6f, 0.25f, 0.9f}, 16, 3.0f, 0.6f,
                           0.13f, 1, 1.5f);
            });
            flash_light(feet + Vec3{0.0f, 1.2f, 0.0f}, Vec3{1.0f, 0.6f, 0.22f}, 4.0f, 10.0f, 0.8f);
            break;
        }
        case SpellId::RockWall: { // the earth rises: an amber ray to the wall line, dust + stones along it
            const Vec3 centre = feet + facing * kRockWallAhead;
            const Vec3 span{-facing.z, 0.0f, facing.x};
            beam(staff, centre + Vec3{0.0f, 0.6f, 0.0f}, Vec4{0.95f, 0.7f, 0.35f, 1.0f}, 0.09f, 0.45f, 0, 30.0f);
            for (int k = 0; k <= 8; ++k) {
                const Vec3 p = centre + span * (kRockWallLength * (static_cast<f32>(k) / 8.0f - 0.5f));
                emit_burst(p + Vec3{0.0f, 0.2f, 0.0f}, Vec4{0.5f, 0.44f, 0.36f, 0.7f}, 5, 2.5f, 0.8f, 0.26f, 0,
                           1.5f, 2.0f);
                emit(p + Vec3{0.0f, 0.3f, 0.0f}, Vec3{frand(-1.0f, 1.0f), frand(3.0f, 6.0f), frand(-1.0f, 1.0f)},
                     Vec4{0.42f, 0.38f, 0.34f, 1.0f}, 0.8f, frand(0.1f, 0.18f), 0, 14.0f, 0.3f);
                emit(p + Vec3{0.0f, 0.25f, 0.0f}, Vec3{0.0f, frand(0.6f, 1.4f), 0.0f},
                     Vec4{1.0f, 0.72f, 0.35f, 0.8f}, 0.7f, 0.3f, 1, -0.3f); // the glowing fault line
            }
            flash_light(centre + Vec3{0.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.7f, 0.35f}, 3.0f, 9.0f, 0.5f);
            break;
        }
        default: break;
    }
}

} // namespace alryn::game
