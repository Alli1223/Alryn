// The from-scratch SFX synthesizer (see Sfx.h). Building blocks: a deterministic xorshift noise
// source, sine/saw oscillators with sweepable frequency, exponential decay envelopes, a one-pole
// low-pass, and a soft clip - enough to voice every clip in the bank without a single asset.

#include <Alryn/Audio/Sfx.h>

#include <algorithm>
#include <cmath>

namespace alryn {
namespace {

constexpr f32 kTau = 6.28318530718f;
constexpr f32 dt() { return 1.0f / static_cast<f32>(kSfxSampleRate); }
inline usize samples(f32 seconds) { return static_cast<usize>(seconds * static_cast<f32>(kSfxSampleRate)); }

// Deterministic noise in [-1, 1] (xorshift32) - the same clip renders identically every run.
struct Noise {
    u32 state = 0x9e3779b9u;
    f32 next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<f32>(state & 0xFFFFFFu) / 8388607.5f - 1.0f;
    }
};

// One-pole low-pass: y += a * (x - y). `cutoff` in Hz (approximate).
struct LowPass {
    f32 y = 0.0f;
    f32 process(f32 x, f32 cutoff) {
        const f32 a = glm::clamp(kTau * cutoff * dt(), 0.0f, 1.0f);
        y += a * (x - y);
        return y;
    }
};

inline f32 soft_clip(f32 x) { return std::tanh(x); }

// Exponential decay from 1 at t=0 with the given half-life-ish rate.
inline f32 decay(f32 t, f32 rate) { return std::exp(-rate * t); }

// Linear attack over `attack` seconds (protects every clip from a start click).
inline f32 attack_env(f32 t, f32 attack) { return attack <= 0.0f ? 1.0f : std::min(t / attack, 1.0f); }

// A sine sweep voice: frequency glides from f0 to f1 over the clip (phase-continuous).
struct Osc {
    f32 phase = 0.0f;
    f32 sine(f32 freq) {
        phase += freq * dt();
        if (phase > 1.0f) {
            phase -= std::floor(phase);
        }
        return std::sin(phase * kTau);
    }
    f32 saw(f32 freq) {
        phase += freq * dt();
        if (phase > 1.0f) {
            phase -= std::floor(phase);
        }
        return phase * 2.0f - 1.0f;
    }
};

// Normalise a rendered buffer to a healthy peak (keeps every clip at a consistent loudness).
void normalise(std::vector<f32>& b, f32 peak_target) {
    f32 peak = 0.0f;
    for (const f32 s : b) {
        peak = std::max(peak, std::abs(s));
    }
    if (peak > 1e-6f) {
        const f32 g = peak_target / peak;
        for (f32& s : b) {
            s *= g;
        }
    }
}

// --- The clip voices -----------------------------------------------------------------------

// A whooshing air cut: band-ish filtered noise whose brightness sweeps up then away.
std::vector<f32> sword_swing() {
    const usize n = samples(0.28f);
    std::vector<f32> b(n);
    Noise rng;
    LowPass lp;
    LowPass hp_track; // subtracting a slow low-pass leaves the airy mid band
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * dt();
        const f32 sweep = 900.0f + 2600.0f * std::sin(std::min(t / 0.28f, 1.0f) * 3.14159f);
        const f32 raw = lp.process(rng.next(), sweep);
        const f32 airy = raw - hp_track.process(raw, 300.0f);
        b[i] = airy * attack_env(t, 0.03f) * decay(t, 9.0f) * 2.2f;
    }
    normalise(b, 0.75f);
    return b;
}

// A meaty thunk: a low sine drop under a short noise crack.
std::vector<f32> sword_hit() {
    const usize n = samples(0.18f);
    std::vector<f32> b(n);
    Noise rng;
    Osc osc;
    LowPass lp;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * dt();
        const f32 body = osc.sine(180.0f - 110.0f * std::min(t / 0.18f, 1.0f)) * decay(t, 22.0f);
        const f32 crack = lp.process(rng.next(), 2400.0f) * decay(t, 70.0f);
        b[i] = soft_clip((body * 1.2f + crack * 0.9f) * attack_env(t, 0.002f) * 1.6f);
    }
    normalise(b, 0.85f);
    return b;
}

// A plucked-string twang (the released bowstring) + a short arrow hiss.
std::vector<f32> bow_shot() {
    const usize n = samples(0.3f);
    std::vector<f32> b(n);
    Noise rng;
    Osc string;
    Osc overtone;
    LowPass lp;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * dt();
        const f32 twang = (string.sine(140.0f) + 0.5f * overtone.sine(283.0f)) * decay(t, 18.0f);
        const f32 hiss = lp.process(rng.next(), 5000.0f) * decay(t, 26.0f) * 0.35f;
        b[i] = soft_clip((twang + hiss) * attack_env(t, 0.002f) * 1.4f);
    }
    normalise(b, 0.8f);
    return b;
}

// A sharp thock: a fast high sine drop with a woody click.
std::vector<f32> arrow_hit() {
    const usize n = samples(0.12f);
    std::vector<f32> b(n);
    Noise rng;
    Osc osc;
    LowPass lp;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * dt();
        const f32 tk = osc.sine(720.0f - 500.0f * std::min(t / 0.12f, 1.0f)) * decay(t, 60.0f);
        const f32 click = lp.process(rng.next(), 3600.0f) * decay(t, 120.0f);
        b[i] = soft_clip((tk + click * 0.6f) * attack_env(t, 0.001f) * 1.5f);
    }
    normalise(b, 0.8f);
    return b;
}

// A shimmering rising chime: two detuned sines gliding up with a sparkle of noise.
std::vector<f32> cast_magic() {
    const usize n = samples(0.5f);
    std::vector<f32> b(n);
    Noise rng;
    Osc a;
    Osc c;
    LowPass lp;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * dt();
        const f32 rise = 500.0f + 900.0f * (t / 0.5f);
        const f32 tone = a.sine(rise) * 0.7f + c.sine(rise * 1.51f) * 0.4f;
        const f32 sparkle = lp.process(rng.next(), 7000.0f) * 0.18f;
        b[i] = (tone + sparkle) * attack_env(t, 0.02f) * decay(t, 5.5f);
    }
    normalise(b, 0.7f);
    return b;
}

// A soft two-note mend: a gentle major third, low then high.
std::vector<f32> heal() {
    const usize n = samples(0.55f);
    std::vector<f32> b(n);
    Osc a;
    Osc c;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * dt();
        const f32 f = t < 0.22f ? 523.25f : 659.25f; // C5 -> E5
        const f32 seg_t = t < 0.22f ? t : t - 0.22f;
        b[i] = (a.sine(f) + 0.3f * c.sine(f * 2.0f)) * attack_env(seg_t, 0.02f) * decay(seg_t, 7.0f) *
               0.8f;
    }
    normalise(b, 0.65f);
    return b;
}

// A bright metallic coin ding: two inharmonic high partials.
std::vector<f32> coin() {
    const usize n = samples(0.3f);
    std::vector<f32> b(n);
    Osc a;
    Osc c;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * dt();
        b[i] = (a.sine(2093.0f) * 0.8f + c.sine(2637.0f) * 0.5f) * attack_env(t, 0.001f) *
               decay(t, 14.0f);
    }
    normalise(b, 0.6f);
    return b;
}

// A low boom: heavily low-passed noise + a sub sine drop, with a longer tail.
std::vector<f32> explosion() {
    const usize n = samples(0.9f);
    std::vector<f32> b(n);
    Noise rng;
    Osc sub;
    LowPass lp;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * dt();
        const f32 rumble = lp.process(rng.next(), 320.0f - 220.0f * std::min(t / 0.9f, 1.0f));
        const f32 drop = sub.sine(90.0f - 55.0f * std::min(t / 0.6f, 1.0f)) * decay(t, 5.0f);
        b[i] = soft_clip((rumble * 1.6f + drop) * attack_env(t, 0.004f) * decay(t, 4.5f) * 2.0f);
    }
    normalise(b, 0.9f);
    return b;
}

// An icy glass burst: bright noise + ringing high partials that die quickly.
std::vector<f32> shatter() {
    const usize n = samples(0.4f);
    std::vector<f32> b(n);
    Noise rng;
    Osc a;
    Osc c;
    LowPass lp;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * dt();
        const f32 glass = lp.process(rng.next(), 9000.0f) * decay(t, 16.0f);
        const f32 ring = (a.sine(3135.0f) + c.sine(4186.0f) * 0.6f) * decay(t, 11.0f) * 0.4f;
        b[i] = (glass + ring) * attack_env(t, 0.002f);
    }
    normalise(b, 0.7f);
    return b;
}

// A war-horn swell: a saw with slow vibrato, rising then held, low-passed to brass.
std::vector<f32> horn() {
    const usize n = samples(1.1f);
    std::vector<f32> b(n);
    Osc osc;
    Osc vib;
    LowPass lp;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * dt();
        const f32 swell = std::min(t / 0.25f, 1.0f);
        const f32 f = 174.6f * (1.0f + 0.012f * vib.sine(5.0f)); // F3 with vibrato
        b[i] = lp.process(osc.saw(f), 900.0f) * swell * decay(std::max(t - 0.55f, 0.0f), 6.0f) * 1.3f;
    }
    normalise(b, 0.75f);
    return b;
}

// A wood crack + clatter: a snap, then a few descending knocks (the wheel bouncing away).
std::vector<f32> wheel_break() {
    const usize n = samples(0.8f);
    std::vector<f32> b(n);
    Noise rng;
    LowPass lp;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * dt();
        const f32 crack = lp.process(rng.next(), 3000.0f) * decay(t, 40.0f) * 1.8f;
        f32 knocks = 0.0f;
        for (int k = 0; k < 4; ++k) { // four fading knocks as the wheel bounces off
            const f32 kt = t - 0.16f - 0.15f * static_cast<f32>(k);
            if (kt > 0.0f) {
                knocks += std::sin(kt * kTau * (240.0f - 30.0f * static_cast<f32>(k))) *
                          decay(kt, 34.0f) * (0.8f - 0.15f * static_cast<f32>(k));
            }
        }
        b[i] = soft_clip((crack + knocks) * attack_env(t, 0.002f));
    }
    normalise(b, 0.85f);
    return b;
}

// A heavy body-slam: a deep thump with a dusty noise skirt.
std::vector<f32> thud() {
    const usize n = samples(0.3f);
    std::vector<f32> b(n);
    Noise rng;
    Osc osc;
    LowPass lp;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * dt();
        const f32 body = osc.sine(110.0f - 70.0f * std::min(t / 0.3f, 1.0f)) * decay(t, 14.0f);
        const f32 dust = lp.process(rng.next(), 700.0f) * decay(t, 20.0f) * 0.5f;
        b[i] = soft_clip((body * 1.5f + dust) * attack_env(t, 0.003f) * 1.5f);
    }
    normalise(b, 0.85f);
    return b;
}

// A long storm rumble: very low noise with a slow amplitude roll and a couple of after-cracks.
std::vector<f32> thunder() {
    const usize n = samples(2.4f);
    std::vector<f32> b(n);
    Noise rng;
    LowPass lp;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * dt();
        const f32 roll = 0.6f + 0.4f * std::sin(t * 5.1f) * std::sin(t * 2.3f); // uneven rolling
        f32 s = lp.process(rng.next(), 240.0f) * roll * 2.0f;
        if (t > 0.7f && t < 0.78f) {
            s += rng.next() * 0.5f; // an after-crack
        }
        b[i] = soft_clip(s * attack_env(t, 0.05f) * decay(t, 1.6f));
    }
    normalise(b, 0.8f);
    return b;
}

// A three-note delivery jingle: a rising major triad, each note a clean chime.
std::vector<f32> fanfare() {
    const usize n = samples(0.8f);
    std::vector<f32> b(n);
    Osc a;
    Osc c;
    const f32 notes[3] = {523.25f, 659.25f, 784.0f}; // C5 E5 G5
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * dt();
        const int note = std::min(static_cast<int>(t / 0.22f), 2);
        const f32 nt = t - 0.22f * static_cast<f32>(note);
        const f32 f = notes[note];
        b[i] = (a.sine(f) + 0.35f * c.sine(f * 2.0f)) * attack_env(nt, 0.01f) * decay(nt, 6.5f) * 0.8f;
    }
    normalise(b, 0.7f);
    return b;
}

// A short dry tick for the UI.
std::vector<f32> ui_click() {
    const usize n = samples(0.05f);
    std::vector<f32> b(n);
    Osc osc;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * dt();
        b[i] = osc.sine(1200.0f) * attack_env(t, 0.001f) * decay(t, 90.0f);
    }
    normalise(b, 0.5f);
    return b;
}

} // namespace

std::vector<f32> render_sfx(SfxId id) {
    switch (id) {
        case SfxId::SwordSwing: return sword_swing();
        case SfxId::SwordHit: return sword_hit();
        case SfxId::BowShot: return bow_shot();
        case SfxId::ArrowHit: return arrow_hit();
        case SfxId::CastMagic: return cast_magic();
        case SfxId::Heal: return heal();
        case SfxId::Coin: return coin();
        case SfxId::Explosion: return explosion();
        case SfxId::Shatter: return shatter();
        case SfxId::Horn: return horn();
        case SfxId::WheelBreak: return wheel_break();
        case SfxId::Thud: return thud();
        case SfxId::Thunder: return thunder();
        case SfxId::Fanfare: return fanfare();
        case SfxId::UiClick: return ui_click();
    }
    return {};
}

} // namespace alryn
