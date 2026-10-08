// The from-scratch SFX synthesizer (see Sfx.h). The DSP kit: seeded white/pink/brown noise, a one-pole
// and a state-variable filter, struck modes (decaying sine partials - metal ones in slightly detuned
// pairs so they shimmer the way real metal does), a Karplus-Strong plucked string, two-operator FM
// bells, band-limited additive brass and a small Freeverb-style room. Each clip LAYERS these: a
// transient for the attack, a body for the weight, a tail for the size - then a touch of space, a DC
// block, a fade so nothing ends on a click, and a normalised peak.

#include <Alryn/Audio/Sfx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

namespace alryn {
namespace {

constexpr f32 kTau = 6.28318530718f;
constexpr f32 kPi = 3.14159265359f;
constexpr f32 kRate = static_cast<f32>(kSfxSampleRate);
constexpr f32 kDt = 1.0f / kRate;
constexpr f32 kLn1000 = 6.90775527898f; // a "t60" decay falls 60 dB (x1/1000) over its time

inline usize samples(f32 seconds) { return static_cast<usize>(std::max(seconds, 0.0f) * kRate); }

// Exponential decay reaching -60 dB at `t60` seconds.
inline f32 decay60(f32 t, f32 t60) { return std::exp(-kLn1000 * t / t60); }
// Smooth 0 -> 1 across [a, b].
inline f32 ramp(f32 t, f32 a, f32 b) { return glm::smoothstep(a, b, t); }
// A struck envelope: a short linear rise (never a start click) into a t60 decay; silent before 0.
inline f32 strike(f32 t, f32 attack, f32 t60) {
    return t < 0.0f ? 0.0f : std::min(t / attack, 1.0f) * decay60(t, t60);
}
// Exponential glide f0 -> f1 over `time` seconds, then held (pitch moves sound natural on a log scale).
inline f32 glide(f32 t, f32 f0, f32 f1, f32 time) { return f0 * std::pow(f1 / f0, std::min(t / time, 1.0f)); }
// An asymmetric bell bump peaking at `at`: rises over ~`rise`, falls over ~`fall` seconds.
inline f32 bump(f32 t, f32 at, f32 rise, f32 fall) {
    const f32 d = t < at ? (t - at) / rise : (t - at) / fall;
    return std::exp(-d * d);
}

// --- Sources ---------------------------------------------------------------------------------

// Deterministic noise (xorshift32), seeded per clip + take - every render is bit-identical.
struct Noise {
    u32 state;
    explicit Noise(u32 seed) : state(seed != 0u ? seed : 0x9e3779b9u) {}
    u32 bits() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    f32 white() { return static_cast<f32>(bits() & 0xFFFFFFu) / 8388607.5f - 1.0f; } // [-1, 1]
    f32 uniform(f32 lo, f32 hi) { return lo + (hi - lo) * static_cast<f32>(bits() & 0xFFFFFFu) / 16777215.0f; }
};

// Pink noise (Paul Kellet's economy filter, -3 dB/octave): softer and fuller than white - air, cloth,
// breath. Roughly white's level through the mids.
struct Pink {
    f32 b0 = 0.0f, b1 = 0.0f, b2 = 0.0f;
    f32 process(f32 white) {
        b0 = 0.99765f * b0 + white * 0.0990460f;
        b1 = 0.96300f * b1 + white * 0.2965164f;
        b2 = 0.57000f * b2 + white * 1.0526913f;
        return (b0 + b1 + b2 + white * 0.1848f) * 0.5f;
    }
};

// Brown noise (leaky-integrated white, -6 dB/octave above ~75 Hz, flat below): the deep body of
// rumbles, blasts and earth - without piling its energy up in the inaudible sub-bass.
struct Brown {
    f32 y = 0.0f;
    f32 process(f32 white) {
        y = (y + white * 0.02f) * 0.99f;
        return y * 6.0f;
    }
};

// --- Filters ---------------------------------------------------------------------------------

struct OnePole {
    f32 y = 0.0f;
    f32 lp(f32 x, f32 cutoff) {
        y += (1.0f - std::exp(-kTau * cutoff * kDt)) * (x - y);
        return y;
    }
    f32 hp(f32 x, f32 cutoff) { return x - lp(x, cutoff); }
};

// A state-variable filter (Simper's trapezoidal SVF): stable under fast cutoff sweeps, resonant.
// `band` is normalised to unity gain at the centre frequency.
struct Svf {
    f32 ic1 = 0.0f, ic2 = 0.0f;
    struct Out {
        f32 low, band, high;
    };
    Out process(f32 x, f32 cutoff, f32 q) {
        const f32 g = std::tan(kPi * std::clamp(cutoff, 20.0f, kRate * 0.45f) * kDt);
        const f32 k = 1.0f / q;
        const f32 a1 = 1.0f / (1.0f + g * (g + k));
        const f32 a2 = g * a1;
        const f32 a3 = g * a2;
        const f32 v3 = x - ic2;
        const f32 v1 = a1 * ic1 + a2 * v3;
        const f32 v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return {v2, v1 * k, x - k * v1 - v2};
    }
};

// --- Tonal voices ----------------------------------------------------------------------------

// A struck partial: a decaying sine ringing from `t0` (0.5 ms rise, so even a ping can't click).
struct Ping {
    f32 t0, freq, amp, t60;
};
f32 ring(const std::vector<Ping>& ps, f32 t) {
    f32 s = 0.0f;
    for (const Ping& p : ps) {
        const f32 u = t - p.t0;
        if (u < 0.0f || u > p.t60) {
            continue;
        }
        s += p.amp * std::sin(kTau * p.freq * u) * std::min(u * 2000.0f, 1.0f) * decay60(u, p.t60);
    }
    return s;
}

// A mode of a struck body: frequency ratio to the fundamental, amplitude and decay.
struct Mode {
    f32 ratio, amp, t60;
};
// Strike a body's modes at `t0`. A `detune` > 0 splits each mode into a slightly detuned pair - the
// slow beating shimmer of real metal (bells, blades, coins).
void strike_modes(std::vector<Ping>& out, f32 t0, f32 f0, std::initializer_list<Mode> modes, f32 gain,
                  f32 detune = 0.0f) {
    for (const Mode& m : modes) {
        if (detune > 0.0f) {
            out.push_back({t0, f0 * m.ratio, gain * m.amp * 0.55f, m.t60});
            out.push_back({t0, f0 * m.ratio * (1.0f + detune), gain * m.amp * 0.45f, m.t60 * 0.85f});
        } else {
            out.push_back({t0, f0 * m.ratio, gain * m.amp, m.t60});
        }
    }
}

// Karplus-Strong plucked string: a noise burst circulating in a delay line one period long, softened
// a little every pass - the bowstring's twang.
struct Pluck {
    std::vector<f32> line;
    usize pos = 0;
    f32 feedback = 0.99f;
    f32 soften = 0.5f;
    Pluck(f32 freq, f32 t60, f32 brightness, Noise& rng) {
        line.resize(std::max<usize>(2, static_cast<usize>(kRate / freq)));
        OnePole lp;
        for (f32& s : line) {
            s = lp.lp(rng.white(), 800.0f + 9000.0f * brightness);
        }
        feedback = std::pow(0.001f, 1.0f / (freq * t60));
        soften = 0.5f - 0.35f * brightness;
    }
    f32 next() {
        const usize j = pos + 1 == line.size() ? 0 : pos + 1;
        const f32 y = line[pos];
        line[pos] = feedback * (y * (1.0f - soften) + line[j] * soften);
        pos = j;
        return y;
    }
};

// Two-operator FM: carrier `freq`, modulator at `ratio` x freq. A decaying index is a bell's bright
// strike mellowing into its hum.
struct Fm {
    f32 pc = 0.0f, pm = 0.0f;
    f32 next(f32 freq, f32 ratio, f32 index) {
        pc += freq * kDt;
        pc -= std::floor(pc);
        pm += freq * ratio * kDt;
        pm -= std::floor(pm);
        return std::sin(kTau * pc + index * std::sin(kTau * pm));
    }
};

// Band-limited brass: a saw summed from its harmonics (none above ~0.42x the sample rate, so it never
// aliases), each rolled off by `bright` (0..1) - brass brightens the harder it's blown.
struct Brass {
    f32 phase = 0.0f;
    f32 next(f32 freq, f32 bright) {
        phase += freq * kDt;
        phase -= std::floor(phase);
        const int nh = std::clamp(static_cast<int>(kRate * 0.42f / freq), 1, 48);
        const f32 x = kTau * phase;
        const f32 c2 = 2.0f * std::cos(x);
        f32 s_prev = 0.0f;
        f32 s_cur = std::sin(x); // sin(h x) by the Chebyshev recurrence - one sin/cos per sample
        f32 w = 1.0f;
        f32 sum = 0.0f;
        for (int h = 1; h <= nh; ++h) {
            sum += s_cur * w / static_cast<f32>(h);
            const f32 s_next = c2 * s_cur - s_prev;
            s_prev = s_cur;
            s_cur = s_next;
            w *= bright;
        }
        return sum * 0.6f;
    }
};

// --- Space + finishing -----------------------------------------------------------------------

// A small Freeverb-style room: six damped feedback combs into three allpasses. A touch of it puts each
// sound IN the world instead of pasted on top of it.
class Reverb {
public:
    Reverb(f32 size, f32 feedback, f32 damp) : feedback_(feedback), damp_(damp) {
        constexpr std::array<usize, 6> kComb{1557, 1617, 1491, 1422, 1277, 1356};
        constexpr std::array<usize, 3> kAllpass{556, 441, 341};
        for (usize k = 0; k < kComb.size(); ++k) {
            combs_[k].buf.assign(std::max<usize>(16, static_cast<usize>(static_cast<f32>(kComb[k]) * size * 1.088f)), 0.0f);
        }
        for (usize k = 0; k < kAllpass.size(); ++k) {
            aps_[k].buf.assign(std::max<usize>(8, static_cast<usize>(static_cast<f32>(kAllpass[k]) * 1.088f)), 0.0f);
        }
    }
    f32 process(f32 x) {
        f32 out = 0.0f;
        for (Line& c : combs_) {
            f32* buf = c.buf.data();
            const f32 y = buf[c.i];
            c.store = y * (1.0f - damp_) + c.store * damp_;
            buf[c.i] = x + c.store * feedback_;
            c.i = c.i + 1 == c.buf.size() ? 0 : c.i + 1;
            out += y;
        }
        out *= 1.0f / 6.0f;
        for (Line& a : aps_) {
            f32* buf = a.buf.data();
            const f32 bv = buf[a.i];
            buf[a.i] = out + bv * 0.5f;
            a.i = a.i + 1 == a.buf.size() ? 0 : a.i + 1;
            out = bv - out;
        }
        return out;
    }

private:
    struct Line {
        std::vector<f32> buf;
        usize i = 0;
        f32 store = 0.0f;
    };
    std::array<Line, 6> combs_;
    std::array<Line, 3> aps_;
    f32 feedback_;
    f32 damp_;
};

// Mix a room under a clip (wet over the dry), extending it by `tail` seconds so the space rings out.
// `size` scales the room, `feedback` sets how long it rings, `damp` how fast its highs die.
void add_space(std::vector<f32>& b, f32 size, f32 feedback, f32 damp, f32 wet, f32 tail) {
    b.resize(b.size() + samples(tail), 0.0f);
    Reverb room(size, feedback, damp);
    OnePole send_hp; // keep the rumble out of the room (a muddy tail is the cheapest-sounding thing)
    f32* o = b.data();
    for (usize i = 0; i < b.size(); ++i) {
        o[i] += room.process(send_hp.hp(o[i], 180.0f)) * wet;
    }
}

// Every clip's last step: high-pass away DC and sub-bass (nothing plays below ~35 Hz, and energy
// there only steals headroom from what you can hear), fade the very end to silence (no stop click),
// and normalise the peak.
void finish(std::vector<f32>& b, f32 peak_target, f32 fade = 0.04f) {
    Svf sub;
    for (f32& s : b) {
        s = sub.process(s, 35.0f, 0.707f).high;
    }
    const usize nf = std::min(b.size(), samples(fade));
    for (usize i = 0; i < nf; ++i) {
        const f32 g = static_cast<f32>(i) / static_cast<f32>(nf);
        b[b.size() - 1 - i] *= g * g;
    }
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

u32 clip_seed(SfxId id, u32 variant) {
    u32 h = 0x9e3779b9u ^ ((static_cast<u32>(id) + 1u) * 0x85ebca6bu);
    h ^= (variant + 1u) * 0xc2b2ae35u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return h != 0u ? h : 1u;
}

// --- The clips -------------------------------------------------------------------------------

// The swing's whoosh, timed to the animation (CharacterAnimator's forehand winds up to ~0.12 s, then
// the cut accelerates through ~0.22 s into the blow at 0.27 s): a quiet mail-and-leather shuffle as
// the arm draws back, then the blade's rush of air - band-passed noise whose loudness AND pitch follow
// the blade's speed (rising, then falling away like a pass-by) - a thin steel whistle at the fastest
// moment, and a low push of displaced air underneath for the weight.
std::vector<f32> sword_swing(u32 v) {
    Noise rng(clip_seed(SfxId::SwordSwing, v));
    const f32 peak = 0.222f + rng.uniform(-0.008f, 0.008f);
    const f32 rise = 0.058f * rng.uniform(0.9f, 1.1f);
    const f32 fall = 0.05f * rng.uniform(0.9f, 1.1f);
    const f32 tone = rng.uniform(0.9f, 1.1f);
    std::vector<Ping> mail; // a few ring-mail chinks as the arm draws back
    for (int k = 0; k < 5; ++k) {
        mail.push_back({rng.uniform(0.05f, 0.13f), rng.uniform(3400.0f, 6800.0f), rng.uniform(0.015f, 0.035f),
                        rng.uniform(0.03f, 0.07f)});
    }
    const usize n = samples(0.5f);
    std::vector<f32> b(n);
    f32* o = b.data();
    Pink pink_a, pink_b;
    Brown brown;
    Svf rustle, body, edge;
    Svf push;
    OnePole flutter;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        const f32 speed = bump(t, peak, rise, fall);
        const f32 w = rng.white();
        const f32 shuffle_env = ramp(t, 0.0f, 0.05f) * (1.0f - ramp(t, 0.1f, 0.17f));
        const f32 flut = 0.55f + 0.45f * std::tanh(flutter.lp(rng.white(), 40.0f) * 10.0f); // cloth flutter
        const f32 shuffle = rustle.process(pink_a.process(w), 1200.0f * tone, 0.7f).band * shuffle_env * flut * 0.3f;
        const f32 rush = body.process(pink_b.process(rng.white()), (320.0f + 1500.0f * speed) * tone, 1.0f + 1.6f * speed).band *
                         std::pow(speed, 1.6f) * 2.2f;
        const f32 whistle = edge.process(w, (2000.0f + 2400.0f * speed) * tone, 8.0f).band * speed * speed * speed * 0.5f;
        const f32 air = push.process(brown.process(rng.white()), 130.0f, 0.8f).band * speed * speed * 1.2f;
        o[i] = shuffle + rush + whistle + air + ring(mail, t) * shuffle_env;
    }
    add_space(b, 0.6f, 0.7f, 0.4f, 0.15f, 0.22f);
    finish(b, 0.8f);
    return b;
}

// A blade biting home: a sharp crack, the meaty THUMP of the body taking it (a falling low sine under
// a leather-and-flesh thwack), a gritty crunch, and the short ring of the steel itself (a blade's
// free-bar modes, shimmering in detuned pairs) - then the room.
std::vector<f32> sword_hit(u32 v) {
    Noise rng(clip_seed(SfxId::SwordHit, v));
    const f32 steel = 760.0f * rng.uniform(0.92f, 1.08f);
    const f32 low = rng.uniform(0.92f, 1.08f);
    std::vector<Ping> ring_modes;
    strike_modes(ring_modes, 0.0f, steel,
                 {{1.0f, 0.24f, 0.45f}, {2.756f, 0.2f, 0.32f}, {5.404f, 0.12f, 0.2f}, {8.933f, 0.06f, 0.1f}},
                 1.0f, 0.006f);
    const usize n = samples(0.45f);
    std::vector<f32> b(n);
    f32* o = b.data();
    Pink pink;
    Svf thwack, grit_bp;
    OnePole crack_hp;
    Fm thump; // ratio 0 = a plain sine; the phase accumulator keeps the drop click-free
    f32 hold = 1.0f;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        const f32 w = rng.white();
        if (i % 96 == 0) {
            hold = rng.uniform(0.25f, 1.0f); // ~2 ms grains of grit
        }
        const f32 crack = crack_hp.hp(w, 1800.0f) * strike(t, 0.0008f, 0.014f) * 1.1f;
        const f32 meat = thwack.process(pink.process(w), 420.0f * low, 0.9f).band * strike(t, 0.001f, 0.13f) * 2.4f;
        const f32 body = thump.next(glide(t, 125.0f, 56.0f, 0.09f) * low, 0.0f, 0.0f) * strike(t, 0.0015f, 0.24f);
        const f32 crunch = grit_bp.process(w, 2600.0f, 0.8f).band * hold * strike(t, 0.001f, 0.06f) * 1.2f;
        o[i] = std::tanh((crack * 1.2f + meat + body * 1.1f + crunch) * 1.4f) + ring(ring_modes, t) * 0.9f;
    }
    add_space(b, 0.7f, 0.72f, 0.35f, 0.2f, 0.32f);
    finish(b, 0.9f);
    return b;
}

// The loosed bowstring: a low, buzzy twang (a plucked string), the slap of the string on the bracer
// and the limbs' thump, then the arrow's fwip tearing away (a falling, receding hiss).
std::vector<f32> bow_shot(u32 v) {
    Noise rng(clip_seed(SfxId::BowShot, v));
    Pluck string(rng.uniform(104.0f, 120.0f), 0.28f, 0.55f, rng);
    const f32 fwip_tone = rng.uniform(0.9f, 1.1f);
    const usize n = samples(0.5f);
    std::vector<f32> b(n);
    f32* o = b.data();
    Pink pink;
    OnePole slap_lp;
    Svf fwip;
    Fm limb;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        const f32 w = rng.white();
        const f32 twang = std::tanh(string.next() * 2.5f) * std::min(t / 0.001f, 1.0f) * 0.9f;
        const f32 slap = slap_lp.lp(pink.process(w), 2200.0f) * strike(t, 0.0008f, 0.035f) * 1.6f;
        const f32 thump = limb.next(78.0f, 0.0f, 0.0f) * strike(t, 0.002f, 0.09f) * 0.6f;
        const f32 ft = t - 0.006f;
        const f32 hiss = fwip.process(w, glide(std::max(ft, 0.0f), 3800.0f, 1100.0f, 0.24f) * fwip_tone, 2.2f).band *
                         strike(ft, 0.012f, 0.28f) * 0.9f;
        o[i] = twang + slap + thump + hiss;
    }
    add_space(b, 0.8f, 0.75f, 0.4f, 0.2f, 0.35f);
    finish(b, 0.82f);
    return b;
}

// An arrow striking home: a sharp tick, the woody THOCK of the head biting in, a dull thud, then the
// shaft quivering - a low thrum that shakes itself out.
std::vector<f32> arrow_hit(u32 v) {
    Noise rng(clip_seed(SfxId::ArrowHit, v));
    const f32 wood = 390.0f * rng.uniform(0.92f, 1.08f);
    std::vector<Ping> thock;
    strike_modes(thock, 0.0f, wood, {{1.0f, 0.6f, 0.09f}, {2.41f, 0.34f, 0.06f}, {4.3f, 0.2f, 0.04f}, {6.8f, 0.1f, 0.025f}}, 1.0f);
    const f32 quiver_hz = rng.uniform(19.0f, 25.0f);
    const usize n = samples(0.45f);
    std::vector<f32> b(n);
    f32* o = b.data();
    OnePole tick_hp;
    Fm thud, shaft, shaft2;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        const f32 tick = tick_hp.hp(rng.white(), 2500.0f) * strike(t, 0.0015f, 0.012f) * 0.9f;
        const f32 dull = thud.next(glide(t, 105.0f, 62.0f, 0.06f), 0.0f, 0.0f) * strike(t, 0.0015f, 0.12f) * 0.55f;
        const f32 qt = t - 0.008f;
        const f32 wobble = 0.5f + 0.5f * std::sin(kTau * quiver_hz * std::max(qt, 0.0f));
        const f32 thrum = (shaft.next(165.0f, 0.0f, 0.0f) + 0.35f * shaft2.next(655.0f, 0.0f, 0.0f)) * wobble *
                          strike(qt, 0.004f, 0.38f) * 0.2f;
        o[i] = tick + ring(thock, t) + dull + thrum;
    }
    add_space(b, 0.7f, 0.72f, 0.4f, 0.15f, 0.28f);
    finish(b, 0.82f);
    return b;
}

// An arcane bolt landing: a falling FM zap, a low whump of force, a crackling fizz, and a scatter of
// sparkles ringing off into the room.
std::vector<f32> spell_hit(u32 v) {
    Noise rng(clip_seed(SfxId::SpellHit, v));
    const f32 tone = rng.uniform(0.92f, 1.08f);
    std::vector<Ping> sparks;
    for (int k = 0; k < 14; ++k) {
        sparks.push_back({rng.uniform(0.01f, 0.32f), rng.uniform(2600.0f, 6400.0f) * tone, rng.uniform(0.03f, 0.08f),
                          rng.uniform(0.08f, 0.2f)});
    }
    const usize n = samples(0.5f);
    std::vector<f32> b(n);
    f32* o = b.data();
    Fm zap, whump;
    Svf fizz;
    f32 gate = 0.0f;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        if (i % 48 == 0) {
            gate = rng.uniform(0.0f, 1.0f) < 0.35f ? 1.0f : 0.0f; // 1 ms cells: a sparse crackle
        }
        const f32 z = zap.next(glide(t, 1500.0f, 260.0f, 0.18f) * tone, 1.5f, 3.5f * decay60(t, 0.25f) + 0.4f) *
                      strike(t, 0.001f, 0.3f) * 0.5f;
        const f32 force = whump.next(glide(t, 150.0f, 52.0f, 0.12f), 0.0f, 0.0f) * strike(t, 0.002f, 0.3f) * 0.85f;
        const f32 crackle = fizz.process(rng.white() * gate, 3200.0f, 0.7f).band * strike(t, 0.001f, 0.26f) * 0.9f;
        o[i] = std::tanh((z + force + crackle) * 1.3f) + ring(sparks, t);
    }
    add_space(b, 0.9f, 0.8f, 0.3f, 0.3f, 0.45f);
    finish(b, 0.85f);
    return b;
}

// A spell gathering and loosing: a rising rush of air under a shimmering FM chord that glides up as it
// builds, with sparkles climbing through it - then a long, airy tail.
std::vector<f32> cast_magic(u32 v) {
    Noise rng(clip_seed(SfxId::CastMagic, v));
    const f32 root = 440.0f * rng.uniform(0.97f, 1.03f);
    std::vector<Ping> sparks;
    for (int k = 0; k < 18; ++k) {
        const f32 at = rng.uniform(0.04f, 0.55f);
        sparks.push_back({at, (2400.0f + 6000.0f * at) * rng.uniform(0.85f, 1.15f), rng.uniform(0.02f, 0.05f),
                          rng.uniform(0.1f, 0.22f)});
    }
    const usize n = samples(0.7f);
    std::vector<f32> b(n);
    f32* o = b.data();
    Pink pink;
    Svf rush;
    Fm v1, v2, v3;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        const f32 env = ramp(t, 0.0f, 0.1f) * (t < 0.32f ? 1.0f : decay60(t - 0.32f, 0.5f));
        const f32 lift = glide(t, 1.0f, 1.32f, 0.42f) * (1.0f + 0.006f * std::sin(kTau * 6.0f * t));
        const f32 index = 1.6f - 1.1f * ramp(t, 0.0f, 0.5f);
        const f32 chord = v1.next(root * lift, 2.0f, index) * 0.35f + v2.next(root * 1.5f * lift, 2.0f, index * 0.8f) * 0.22f +
                          v3.next(root * 2.0f * lift, 2.0f, index * 0.6f) * 0.14f;
        const f32 air = rush.process(pink.process(rng.white()), glide(t, 380.0f, 2600.0f, 0.38f), 1.4f).band *
                        ramp(t, 0.0f, 0.16f) * (1.0f - ramp(t, 0.3f, 0.6f)) * 0.9f;
        o[i] = chord * env + air + ring(sparks, t);
    }
    add_space(b, 1.0f, 0.82f, 0.3f, 0.32f, 0.55f);
    finish(b, 0.72f);
    return b;
}

// A mend: a warm, gentle major chord rolled up as soft FM bells (C E G C), over a quiet low pad, with
// a breath of air - generous room so it glows rather than pings.
std::vector<f32> heal(u32 v) {
    Noise rng(clip_seed(SfxId::Heal, v));
    constexpr std::array<f32, 4> kNotes{523.25f, 659.25f, 784.0f, 1046.5f}; // C5 E5 G5 C6
    const usize n = samples(0.9f);
    std::vector<f32> b(n);
    f32* o = b.data();
    std::array<Fm, 4> bells;
    Fm pad1, pad2;
    Pink pink;
    Svf breath;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        f32 s = 0.0f;
        for (usize k = 0; k < kNotes.size(); ++k) {
            const f32 nt = t - 0.07f * static_cast<f32>(k);
            if (nt < 0.0f) {
                continue;
            }
            const f32 vib = 1.0f + 0.004f * std::sin(kTau * 5.0f * nt);
            s += bells[k].next(kNotes[k] * vib, 2.0f, 0.9f * decay60(nt, 0.4f) + 0.15f) * strike(nt, 0.015f, 1.0f) * 0.3f;
        }
        const f32 pad = (pad1.next(261.63f, 0.0f, 0.0f) + 0.6f * pad2.next(392.0f, 0.0f, 0.0f)) * ramp(t, 0.0f, 0.18f) *
                        decay60(t, 1.3f) * 0.13f;
        const f32 air = breath.process(pink.process(rng.white()), 5500.0f, 0.9f).band * bump(t, 0.2f, 0.12f, 0.3f) * 0.08f;
        o[i] = s + pad + air;
    }
    add_space(b, 1.0f, 0.84f, 0.25f, 0.38f, 0.7f);
    finish(b, 0.66f);
    return b;
}

// Coins: a bright clink as one strikes another (a coin's inharmonic modes, shimmering in pairs), a
// second answering clink and a little settling chink.
std::vector<f32> coin(u32 v) {
    Noise rng(clip_seed(SfxId::Coin, v));
    const f32 f0 = 2350.0f * rng.uniform(0.94f, 1.06f);
    const f32 t2 = rng.uniform(0.06f, 0.09f);
    const f32 t3 = t2 + rng.uniform(0.045f, 0.07f);
    std::vector<Ping> modes;
    const std::initializer_list<Mode> kCoin{{1.0f, 0.5f, 0.38f}, {1.58f, 0.36f, 0.3f}, {2.31f, 0.24f, 0.2f}, {3.04f, 0.14f, 0.12f}};
    strike_modes(modes, 0.0f, f0, kCoin, 1.0f, 0.0025f);
    strike_modes(modes, t2, f0 * 1.07f, kCoin, 0.6f, 0.0025f);
    strike_modes(modes, t3, f0 * 0.95f, kCoin, 0.28f, 0.0025f);
    const usize n = samples(0.55f);
    std::vector<f32> b(n);
    f32* o = b.data();
    OnePole click_hp;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        const f32 click = click_hp.hp(rng.white(), 4000.0f) *
                          (strike(t, 0.0006f, 0.005f) + 0.6f * strike(t - t2, 0.0006f, 0.005f) + 0.3f * strike(t - t3, 0.0006f, 0.005f));
        o[i] = ring(modes, t) + click * 0.5f;
    }
    add_space(b, 0.5f, 0.7f, 0.3f, 0.16f, 0.25f);
    finish(b, 0.62f);
    return b;
}

// A blast: a split-second crack, a deep sub boom falling away, a roaring body of brown noise whose
// brightness closes down as it fades, and rubble pattering down after - in a big, damped space.
std::vector<f32> explosion(u32 v) {
    Noise rng(clip_seed(SfxId::Explosion, v));
    std::vector<Ping> debris;
    for (int k = 0; k < 40; ++k) {
        const f32 u = rng.uniform(0.0f, 1.0f);
        debris.push_back({0.08f + 1.1f * u * u, rng.uniform(700.0f, 3600.0f), rng.uniform(0.03f, 0.12f) * (1.0f - 0.6f * u),
                          rng.uniform(0.015f, 0.05f)});
    }
    const usize n = samples(1.4f);
    std::vector<f32> b(n);
    f32* o = b.data();
    Brown brown;
    OnePole roar_lp, crack_lp;
    Fm sub;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        const f32 w = rng.white();
        const f32 crack = crack_lp.lp(w, 6000.0f) * strike(t, 0.0008f, 0.04f) * 0.9f;
        const f32 roar = roar_lp.lp(brown.process(w), glide(t, 1800.0f, 110.0f, 1.0f)) * strike(t, 0.004f, 1.4f) * 1.5f;
        const f32 boom = sub.next(glide(t, 72.0f, 40.0f, 0.5f), 0.0f, 0.0f) * strike(t, 0.003f, 0.75f);
        o[i] = std::tanh((crack + roar + boom * 1.2f) * 1.6f) + ring(debris, t);
    }
    add_space(b, 1.2f, 0.85f, 0.55f, 0.32f, 0.9f);
    finish(b, 0.95f);
    return b;
}

// Ice shattering: a bright crack and a crunch, then a spray of glassy shards (dozens of tiny, high,
// fast-dying pings, thick at first and thinning out), and a few last pieces tinkling down.
std::vector<f32> shatter(u32 v) {
    Noise rng(clip_seed(SfxId::Shatter, v));
    std::vector<Ping> shards;
    for (int k = 0; k < 70; ++k) {
        const f32 u = rng.uniform(0.0f, 1.0f);
        const f32 at = 0.32f * u * u;
        shards.push_back({at, rng.uniform(2200.0f, 9000.0f), rng.uniform(0.05f, 0.18f) * (1.0f - 0.7f * u),
                          rng.uniform(0.04f, 0.18f)});
    }
    for (int k = 0; k < 7; ++k) {
        shards.push_back({rng.uniform(0.3f, 0.6f), rng.uniform(3000.0f, 7500.0f), rng.uniform(0.03f, 0.07f), rng.uniform(0.08f, 0.2f)});
    }
    const usize n = samples(0.7f);
    std::vector<f32> b(n);
    f32* o = b.data();
    OnePole crack_hp;
    Pink pink;
    Svf crunch_bp;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        const f32 w = rng.white();
        const f32 crack = crack_hp.hp(w, 3000.0f) * strike(t, 0.0007f, 0.025f) * 0.8f;
        const f32 crunch = crunch_bp.process(pink.process(w), 1300.0f, 0.8f).band * strike(t, 0.001f, 0.09f) * 1.2f;
        o[i] = crack + crunch + ring(shards, t);
    }
    add_space(b, 0.9f, 0.8f, 0.2f, 0.3f, 0.55f);
    finish(b, 0.72f);
    return b;
}

// A war horn: two band-limited brass voices a hair apart (an ensemble, not a synth), scooping up into
// the note, brightening as the swell builds, vibrato blooming once it's held, a breath of air in it -
// and a big outdoor space for the call to carry across.
std::vector<f32> horn(u32 v) {
    Noise rng(clip_seed(SfxId::Horn, v));
    const usize n = samples(1.5f);
    std::vector<f32> b(n);
    f32* o = b.data();
    Brass h1, h2;
    Pink pink;
    Svf breath, body;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        const f32 env = ramp(t, 0.0f, 0.22f) * (t < 1.05f ? 1.0f : decay60(t - 1.05f, 0.45f));
        const f32 scoop = std::exp2((-1.0f + ramp(t, 0.0f, 0.14f)) / 12.0f);     // a semitone up into the note
        const f32 vib = 1.0f + 0.007f * ramp(t, 0.35f, 0.8f) * std::sin(kTau * 5.2f * t);
        const f32 f = 174.61f * scoop * vib;                                    // F3
        const f32 bright = 0.5f + 0.38f * env;
        const f32 tone = h1.next(f, bright) + 0.8f * h2.next(f * 1.004f, bright * 0.97f);
        const Svf::Out shaped = body.process(tone, 2200.0f, 0.75f);
        const f32 air = breath.process(pink.process(rng.white()), 1300.0f, 1.0f).band * 0.05f;
        o[i] = (shaped.low + air) * env;
    }
    add_space(b, 1.3f, 0.86f, 0.45f, 0.42f, 1.1f);
    finish(b, 0.78f);
    return b;
}

// A wheel shearing off: a sharp wooden crack (the spokes' modes + splinters), then the wheel bouncing
// away - each knock lower and closer to the last, as a bouncing thing does - and a short roll.
std::vector<f32> wheel_break(u32 v) {
    Noise rng(clip_seed(SfxId::WheelBreak, v));
    std::vector<Ping> wood;
    strike_modes(wood, 0.0f, 310.0f, {{1.0f, 0.55f, 0.13f}, {2.4f, 0.38f, 0.09f}, {4.16f, 0.26f, 0.06f}, {6.8f, 0.15f, 0.04f}}, 1.6f);
    for (int k = 0; k < 24; ++k) {
        wood.push_back({rng.uniform(0.0f, 0.12f), rng.uniform(1500.0f, 5000.0f), rng.uniform(0.04f, 0.1f), rng.uniform(0.01f, 0.025f)});
    }
    constexpr std::array<f32, 6> kGap{0.0f, 0.2f, 0.15f, 0.11f, 0.08f, 0.06f};
    std::array<f32, 6> knock_t{};
    f32 at = 0.22f;
    for (usize k = 0; k < kGap.size(); ++k) {
        at += kGap[k];
        knock_t[k] = at;
        const f32 g = 0.5f * std::pow(0.72f, static_cast<f32>(k));
        strike_modes(wood, at, 220.0f * rng.uniform(0.95f, 1.05f), {{1.0f, 0.5f, 0.08f}, {2.45f, 0.3f, 0.06f}, {4.4f, 0.16f, 0.04f}}, g);
    }
    const f32 roll_t = knock_t.back();
    const usize n = samples(1.15f);
    std::vector<f32> b(n);
    f32* o = b.data();
    OnePole crack_lp, roll_lp;
    Brown brown;
    std::array<Fm, 6> thumps;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        const f32 w = rng.white();
        f32 knocks = 0.0f;
        for (usize k = 0; k < knock_t.size(); ++k) {
            const f32 kt = t - knock_t[k];
            if (kt >= 0.0f && kt < 0.12f) {
                knocks += thumps[k].next(glide(kt, 140.0f, 90.0f, 0.05f), 0.0f, 0.0f) * strike(kt, 0.0015f, 0.08f) * 0.45f *
                          std::pow(0.72f, static_cast<f32>(k));
            }
        }
        const f32 crack = crack_lp.lp(w, 5000.0f) * strike(t, 0.0007f, 0.05f) * 1.6f;
        const f32 rt = t - roll_t;
        const f32 roll = rt > 0.0f ? roll_lp.lp(brown.process(w), 420.0f) * (0.6f + 0.4f * std::sin(kTau * 17.0f * rt)) *
                                         strike(rt, 0.02f, 0.4f) * 0.5f
                                   : 0.0f;
        o[i] = std::tanh((crack + knocks + roll) * 1.2f) + ring(wood, t);
    }
    add_space(b, 0.9f, 0.75f, 0.45f, 0.2f, 0.45f);
    finish(b, 0.86f);
    return b;
}

// A heavy body hitting the ground: a deep falling thump, the dull slap of impact, earth and dust
// settling, a few clods skittering.
std::vector<f32> thud(u32 v) {
    Noise rng(clip_seed(SfxId::Thud, v));
    std::vector<Ping> clods;
    for (int k = 0; k < 8; ++k) {
        clods.push_back({rng.uniform(0.02f, 0.25f), rng.uniform(450.0f, 1500.0f), rng.uniform(0.03f, 0.07f), rng.uniform(0.02f, 0.05f)});
    }
    const usize n = samples(0.5f);
    std::vector<f32> b(n);
    f32* o = b.data();
    Pink pink;
    Brown brown;
    OnePole slap_lp, dust_lp;
    Fm sub;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        const f32 w = rng.white();
        const f32 body = sub.next(glide(t, 92.0f, 46.0f, 0.15f), 0.0f, 0.0f) * strike(t, 0.002f, 0.3f);
        const f32 slap = slap_lp.lp(pink.process(w), 650.0f) * strike(t, 0.001f, 0.08f) * 1.6f;
        const f32 dust = dust_lp.lp(brown.process(w), 320.0f) * strike(t, 0.01f, 0.28f) * 0.6f;
        o[i] = std::tanh((body * 1.3f + slap + dust) * 1.5f) + ring(clods, t);
    }
    add_space(b, 0.8f, 0.72f, 0.5f, 0.16f, 0.35f);
    finish(b, 0.86f);
    return b;
}

// Thunder: a tearing crack (sizzling noise, gated into a ripping texture), then the long roll - deep
// brown-noise rumble swelling and fading in uneven waves as the sound arrives from farther along the
// bolt - in a huge, dark space.
std::vector<f32> thunder(u32 v) {
    Noise rng(clip_seed(SfxId::Thunder, v));
    std::array<f32, 4> roll_at{};
    for (f32& r : roll_at) {
        r = rng.uniform(0.5f, 2.1f);
    }
    const usize n = samples(3.0f);
    std::vector<f32> b(n);
    f32* o = b.data();
    Brown brown;
    Pink pink;
    OnePole crack_lp, rumble_lp, mid_lp, wander;
    Svf chest;
    f32 gate = 1.0f;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        const f32 w = rng.white();
        if (i % 160 == 0) {
            gate = rng.uniform(0.2f, 1.0f);
        }
        const f32 crack = crack_lp.lp(w, 4500.0f) * gate * (strike(t, 0.002f, 0.32f) + 0.7f * strike(t - 0.06f, 0.002f, 0.25f)) * 0.8f;
        f32 rolls = 0.6f + 0.6f * std::tanh(wander.lp(rng.white(), 2.0f) * 30.0f); // slow, uneven swells
        for (const f32 r : roll_at) {
            rolls += 0.7f * bump(t, r, 0.12f, 0.3f);
        }
        const f32 env = ramp(t, 0.0f, 0.25f) * decay60(t, 3.2f) * rolls;
        const f32 low = brown.process(w);
        const f32 rumble = (rumble_lp.lp(low, 170.0f) * 1.3f + chest.process(low, 75.0f, 0.9f).band * 1.4f) * env;
        const f32 mid = mid_lp.lp(pink.process(rng.white()), 700.0f) * env * 0.3f;
        o[i] = std::tanh((crack + rumble + mid) * 1.3f);
    }
    add_space(b, 1.4f, 0.88f, 0.6f, 0.45f, 1.3f);
    finish(b, 0.85f, 0.2f);
    return b;
}

// A delivery fanfare: a brass "ta-ta-taaa" (C5, E5, then a held G5 that blooms into a C-major chord),
// tongued attacks and swelling brightness, in a hall-sized space.
std::vector<f32> fanfare(u32 v) {
    (void)v;
    struct Note {
        f32 at, freq, len, amp;
    };
    constexpr std::array<Note, 5> kNotes{{
        {0.0f, 523.25f, 0.12f, 0.8f},  // C5
        {0.14f, 659.25f, 0.12f, 0.8f}, // E5
        {0.28f, 784.0f, 0.62f, 1.0f},  // G5, held...
        {0.3f, 659.25f, 0.6f, 0.45f},  // ...over E5
        {0.32f, 523.25f, 0.58f, 0.4f}, // ...and C5 - the chord
    }};
    const usize n = samples(1.05f);
    std::vector<f32> b(n);
    f32* o = b.data();
    std::array<Brass, 5> voices;
    Svf tone;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        f32 s = 0.0f;
        for (usize k = 0; k < kNotes.size(); ++k) {
            const Note& nt = kNotes[k];
            const f32 u = t - nt.at;
            if (u < 0.0f) {
                continue;
            }
            const f32 env = ramp(u, 0.0f, 0.025f) * (u < nt.len ? 1.0f - 0.25f * ramp(u, 0.03f, nt.len) : decay60(u - nt.len, 0.18f));
            const f32 scoop = std::exp2((-0.4f + 0.4f * ramp(u, 0.0f, 0.04f)) / 12.0f);
            const f32 vib = 1.0f + 0.005f * ramp(u, 0.25f, 0.5f) * std::sin(kTau * 5.5f * u);
            s += voices[k].next(nt.freq * scoop * vib, 0.45f + 0.35f * env) * env * nt.amp;
        }
        o[i] = tone.process(s, 3800.0f, 0.7f).low * 0.5f;
    }
    add_space(b, 1.1f, 0.84f, 0.4f, 0.34f, 0.75f);
    finish(b, 0.72f);
    return b;
}

// A UI tick: a small, crisp, woody click - two short high modes over a soft body knock. Dry (menus
// don't live in the world).
std::vector<f32> ui_click(u32 v) {
    Noise rng(clip_seed(SfxId::UiClick, v));
    std::vector<Ping> modes;
    strike_modes(modes, 0.0f, 2100.0f, {{1.0f, 0.55f, 0.022f}, {1.76f, 0.32f, 0.016f}, {2.48f, 0.18f, 0.011f}}, 1.0f);
    modes.push_back({0.0f, 430.0f, 0.3f, 0.03f});
    const usize n = samples(0.06f);
    std::vector<f32> b(n);
    f32* o = b.data();
    OnePole hp;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        o[i] = ring(modes, t) + hp.hp(rng.white(), 3000.0f) * strike(t, 0.001f, 0.004f) * 0.35f;
    }
    finish(b, 0.5f, 0.01f);
    return b;
}

// A level-up: a quick rising arpeggio of bright FM bells (C E G C), the last one ringing on as a soft
// chord swells beneath it, a whoosh rising through and sparkles climbing to the top.
std::vector<f32> level_up(u32 v) {
    Noise rng(clip_seed(SfxId::LevelUp, v));
    constexpr std::array<f32, 4> kNotes{523.25f, 659.25f, 784.0f, 1046.5f}; // C5 E5 G5 C6
    std::vector<Ping> sparks;
    for (int k = 0; k < 30; ++k) {
        const f32 at = rng.uniform(0.0f, 0.95f);
        sparks.push_back({at, (2000.0f + 5000.0f * at) * rng.uniform(0.85f, 1.15f), rng.uniform(0.015f, 0.04f), rng.uniform(0.12f, 0.3f)});
    }
    const usize n = samples(1.4f);
    std::vector<f32> b(n);
    f32* o = b.data();
    std::array<Fm, 4> bells;
    std::array<Fm, 4> pad;
    Pink pink;
    Svf rush;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        f32 s = 0.0f;
        for (usize k = 0; k < kNotes.size(); ++k) {
            const f32 nt = t - 0.09f * static_cast<f32>(k);
            if (nt < 0.0f) {
                continue;
            }
            const f32 tail = k == 3 ? 1.6f : 0.55f;
            s += bells[k].next(kNotes[k], 2.0f, 2.2f * decay60(nt, 0.35f) + 0.25f) * strike(nt, 0.004f, tail) * 0.33f;
        }
        const f32 ct = t - 0.27f;
        if (ct > 0.0f) {
            const f32 trem = 1.0f + 0.15f * std::sin(kTau * 4.5f * ct);
            f32 chord = 0.0f;
            for (usize k = 0; k < kNotes.size(); ++k) {
                chord += pad[k].next(kNotes[k] * 0.5f * (1.0f + 0.0015f * static_cast<f32>(k)), 1.0f, 0.3f);
            }
            s += chord * ramp(ct, 0.0f, 0.22f) * decay60(ct, 1.5f) * trem * 0.08f;
        }
        const f32 air = rush.process(pink.process(rng.white()), glide(t, 600.0f, 5200.0f, 0.42f), 1.3f).band *
                        ramp(t, 0.0f, 0.1f) * (1.0f - ramp(t, 0.3f, 0.6f)) * 0.35f;
        o[i] = s + air + ring(sparks, t);
    }
    add_space(b, 1.1f, 0.85f, 0.3f, 0.36f, 0.9f);
    finish(b, 0.72f);
    return b;
}

} // namespace

// A raider's war-cry / a beast's snarl: a buzzing vocal source (band-limited, so it's a throat not a
// synth) with a ragged growl - its loudness chopped ~30 times a second, the way a roar rattles - pushed
// through two vowel formants that slide from a snarled "rrr" open into "AARGH", a breath of hiss on
// top, the pitch heaving up then sagging; a little room so it carries across the field.
std::vector<f32> roar(u32 v) {
    Noise rng(clip_seed(SfxId::Roar, v));
    const f32 f0 = rng.uniform(88.0f, 112.0f);
    const f32 dur = rng.uniform(0.75f, 0.95f);
    const f32 growl_rate = rng.uniform(26.0f, 34.0f);
    const usize n = samples(dur);
    std::vector<f32> b(n);
    f32* o = b.data();
    Brass voice;
    Pink breath;
    Svf form1, form2, hiss_bp;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        const f32 u = t / dur;
        const f32 env = ramp(t, 0.0f, 0.06f) * (1.0f - ramp(u, 0.62f, 1.0f));
        // The pitch heaves up into the cry and sags as the breath runs out (plus a ragged wobble).
        const f32 pitch = f0 * (1.0f + 0.45f * bump(u, 0.3f, 0.25f, 0.5f)) *
                          (1.0f + 0.03f * std::sin(kTau * 6.0f * t) + 0.02f * rng.white());
        const f32 growl = 0.55f + 0.45f * std::abs(std::sin(kPi * growl_rate * t + 0.6f * rng.white()));
        const f32 src = voice.next(pitch, 0.86f) * growl;
        const f32 w = rng.white();
        // Vowel formants: a closed snarl opening into a wide-mouthed bellow.
        const f32 f1 = glm::mix(380.0f, 720.0f, ramp(u, 0.05f, 0.35f));
        const f32 f2 = glm::mix(900.0f, 1250.0f, ramp(u, 0.05f, 0.35f));
        const f32 voiced = form1.process(src, f1, 3.2f).band * 1.1f + form2.process(src, f2, 4.0f).band * 0.65f +
                           src * 0.12f;
        const f32 hiss = hiss_bp.process(breath.process(w), 1900.0f, 0.8f).band * 0.35f;
        o[i] = std::tanh((voiced + hiss) * env * 2.2f);
    }
    add_space(b, 0.85f, 0.7f, 0.5f, 0.14f, 0.3f);
    finish(b, 0.8f, 0.06f);
    return b;
}

// A dire wolf's howl: a pure, slightly hollow voice that slides up from a low whine to a long held note
// and falls away, with a slow vibrato, a breath under it, a crack of a yip at the top - far off, in a
// big open space.
std::vector<f32> howl(u32 v) {
    Noise rng(clip_seed(SfxId::Howl, v));
    const f32 lo = rng.uniform(300.0f, 360.0f);
    const f32 hi = lo * rng.uniform(1.65f, 1.85f);
    const f32 dur = rng.uniform(1.6f, 1.9f);
    const usize n = samples(dur);
    std::vector<f32> b(n);
    f32* o = b.data();
    Pink breath;
    OnePole breath_lp;
    f32 ph = 0.0f;
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        const f32 u = t / dur;
        const f32 env = ramp(t, 0.0f, 0.18f) * (1.0f - ramp(u, 0.7f, 1.0f));
        f32 pitch = glm::mix(lo, hi, ramp(u, 0.04f, 0.3f));
        pitch *= 1.0f - 0.22f * ramp(u, 0.62f, 1.0f);              // falling away at the end
        pitch *= 1.0f + 0.012f * std::sin(kTau * 5.2f * t) * ramp(u, 0.2f, 0.4f); // vibrato on the hold
        ph += pitch * kDt;
        ph -= std::floor(ph);
        const f32 x = kTau * ph;
        // Mostly fundamental with a hollow 2nd + 3rd (a throat, not a whistle).
        const f32 voice = std::sin(x) + 0.32f * std::sin(2.0f * x) + 0.12f * std::sin(3.0f * x);
        const f32 air = breath_lp.lp(breath.process(rng.white()), 2200.0f) * 0.12f;
        o[i] = (voice * 0.7f + air) * env;
    }
    add_space(b, 1.25f, 0.84f, 0.35f, 0.32f, 0.9f);
    finish(b, 0.7f, 0.12f);
    return b;
}

// A spade biting into earth: the crunch of the blade going in (soil grit - gated, low-passed noise in
// a quick burst of crackles), a dull thump of the clod coming loose, then a short gritty scrape.
std::vector<f32> dig(u32 v) {
    Noise rng(clip_seed(SfxId::Dig, v));
    const usize n = samples(0.42f);
    std::vector<f32> b(n);
    f32* o = b.data();
    Brown brown;
    Pink pink;
    OnePole crunch_lp, scrape_lp, scrape_hp;
    Fm thump;
    const f32 scrape_at = rng.uniform(0.12f, 0.16f);
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) * kDt;
        const f32 w = rng.white();
        // Grit: noise gated into crackles, densest as the blade drives in.
        const f32 grit_gate = (rng.uniform(0.0f, 1.0f) < 0.35f ? 1.0f : 0.25f);
        const f32 crunch = crunch_lp.lp(w * grit_gate, 2600.0f) * strike(t, 0.003f, 0.13f) * 1.4f;
        const f32 body = (thump.next(glide(t, 140.0f, 70.0f, 0.08f), 0.0f, 0.0f) * 0.8f + brown.process(w) * 0.4f) *
                         strike(t, 0.004f, 0.16f);
        const f32 scrape =
            scrape_hp.hp(scrape_lp.lp(pink.process(w), 3400.0f), 700.0f) * bump(t, scrape_at + 0.07f, 0.05f, 0.09f) * 0.7f;
        o[i] = std::tanh((crunch + body + scrape) * 1.6f);
    }
    add_space(b, 0.6f, 0.6f, 0.6f, 0.08f, 0.15f);
    finish(b, 0.78f);
    return b;
}

u32 sfx_variants(SfxId id) {
    switch (id) {
        case SfxId::SwordSwing:
        case SfxId::SwordHit: return 4;
        case SfxId::BowShot:
        case SfxId::ArrowHit:
        case SfxId::SpellHit:
        case SfxId::Coin:
        case SfxId::CastMagic: return 3;
        case SfxId::Dig: return 3;
        case SfxId::Thud:
        case SfxId::Shatter:
        case SfxId::Roar:
        case SfxId::Howl:
        case SfxId::Thunder: return 2;
        default: return 1;
    }
}

std::vector<f32> render_sfx(SfxId id, u32 variant) {
    switch (id) {
        case SfxId::SwordSwing: return sword_swing(variant);
        case SfxId::SwordHit: return sword_hit(variant);
        case SfxId::BowShot: return bow_shot(variant);
        case SfxId::ArrowHit: return arrow_hit(variant);
        case SfxId::CastMagic: return cast_magic(variant);
        case SfxId::Heal: return heal(variant);
        case SfxId::Coin: return coin(variant);
        case SfxId::Explosion: return explosion(variant);
        case SfxId::Shatter: return shatter(variant);
        case SfxId::Horn: return horn(variant);
        case SfxId::WheelBreak: return wheel_break(variant);
        case SfxId::Thud: return thud(variant);
        case SfxId::Thunder: return thunder(variant);
        case SfxId::Fanfare: return fanfare(variant);
        case SfxId::UiClick: return ui_click(variant);
        case SfxId::LevelUp: return level_up(variant);
        case SfxId::SpellHit: return spell_hit(variant);
        case SfxId::Roar: return roar(variant);
        case SfxId::Howl: return howl(variant);
        case SfxId::Dig: return dig(variant);
    }
    return {};
}

} // namespace alryn
