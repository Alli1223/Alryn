#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

#include <vector>

// The sound-effect bank, synthesized FROM SCRATCH - no audio assets, in the same spirit as the
// vector font: every clip is rendered procedurally (oscillators + noise + envelopes + simple
// filters) into a mono f32 PCM buffer at engine startup. Pure DSP with a deterministic seed, so
// the synth is headless-testable and the whole soundscape ships inside the binary.
namespace alryn {

enum class SfxId : u8 {
    SwordSwing = 0, // a whooshing air cut (also pitched for the ally-toss flight)
    SwordHit,       // a meaty thunk when a blow lands
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
};
inline constexpr usize kSfxCount = 15;

// The shared engine sample rate for synthesis + playback (miniaudio converts to native).
inline constexpr u32 kSfxSampleRate = 48000;

// Render one clip: a mono f32 buffer in [-1, 1] at kSfxSampleRate. Deterministic per id.
std::vector<f32> render_sfx(SfxId id);

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
