#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

#include <vector>

// The sound-effect bank, synthesized FROM SCRATCH - no audio assets, in the same spirit as the
// vector font: every clip is rendered procedurally into a mono f32 PCM buffer at engine startup.
// Each one is layered like a designed sound (a transient for the attack, a body for the weight, a
// tail for the size) from a small DSP kit: seeded noise, resonant filters, struck metal/wood modes,
// a plucked string, FM bells, band-limited brass and a little reverb. Pure DSP with deterministic
// seeds, so the synth is headless-testable and the whole soundscape ships inside the binary.
namespace alryn {

enum class SfxId : u8 {
    SwordSwing = 0, // the swing's whoosh, timed to the animation's cut (peaks ~0.22 s in)
    SwordHit,       // a blade biting home: crack + meaty thump + a ring of steel
    BowShot,        // a plucked-string twang + arrow hiss
    ArrowHit,       // a sharp thock (arrow striking wood/flesh)
    CastMagic,      // a shimmering rising chime (ability / spell casts)
    Heal,           // a soft two-note mend chime
    Coin,           // a bright metallic ding (loot / pay landing)
    Explosion,      // a low boom (sapper blast, wreck)
    Shatter,        // icy glass burst (the frost-shatter combo)
    Horn,           // a war-horn swell (the Hunter's rally)
    WheelBreak,     // a wood crack + clatter (the wheel shearing off)
    Thud,           // a heavy body-slam (the toss cannonball landing)
    Thunder,        // a long storm rumble (follows the lightning flash)
    Fanfare,        // a three-note delivery jingle
    UiClick,        // a short dry tick for menu/button presses
    LevelUp,        // a bright rising arpeggio + shimmer (a hero levels up / a journey step lands)
    SpellHit,       // an arcane bolt landing: a zap, a whump and a fizz of sparks
    Roar,           // a raider's war-cry / a beast's snarl: a rough, throaty bellow
    Howl,           // a dire wolf's howl rising out of the wilds
    Dig,            // a spade biting into earth: a crunch of soil + a gritty scrape
};
inline constexpr usize kSfxCount = 20;

// The shared engine sample rate for synthesis + playback (miniaudio converts to native).
inline constexpr u32 kSfxSampleRate = 48000;

// How many distinct takes the bank holds of a clip. The frequent combat sounds get a few (each with
// its own noise and small detunes) and playback rotates through them, so a flurry of swings doesn't
// machine-gun one identical sample.
u32 sfx_variants(SfxId id);

// Render one take of a clip: a mono f32 buffer in [-1, 1] at kSfxSampleRate. Deterministic per
// (id, variant).
std::vector<f32> render_sfx(SfxId id, u32 variant = 0);

// --- 3D spatialisation (pure, shared by the mixer and the tests) --------------------------------
// Per-ear gains for a sound at `source` heard by a listener at `listener` facing `listener_yaw`
// (xz heading, same convention as player yaw): distance attenuation with a far cutoff, and an
// equal-power left/right pan from the signed bearing to the source.
struct StereoGain {
    f32 left = 0.0f;
    f32 right = 0.0f;
};
inline constexpr f32 kSfxMaxDistance = 55.0f; // beyond this a 3D sound is culled entirely

inline StereoGain spatialize(const Vec3& listener, f32 listener_yaw, const Vec3& source,
                             f32 gain = 1.0f) {
    Vec3 to = source - listener;
    to.y *= 0.5f; // height matters less than ground distance for an iso camera
    const f32 d = glm::length(to);
    if (d >= kSfxMaxDistance) {
        return {};
    }
    const f32 atten = 1.0f / (1.0f + d * d * 0.012f);
    // Signed bearing of the source relative to the listener's facing: positive = to their right.
    f32 pan = 0.0f;
    if (d > 0.5f) {
        const Vec2 fwd{std::cos(listener_yaw), std::sin(listener_yaw)};
        const Vec2 dir{to.x / d, to.z / d};
        pan = fwd.x * dir.y - fwd.y * dir.x; // 2D cross: sin of the bearing angle
    }
    // Equal-power pan, never fully one-eared (the world stays present in both ears).
    const f32 p = glm::clamp(pan, -1.0f, 1.0f) * 0.6f;
    const f32 g = gain * atten;
    return {g * std::sqrt(0.5f * (1.0f - p)), g * std::sqrt(0.5f * (1.0f + p))};
}

} // namespace alryn
