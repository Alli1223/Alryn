#include <doctest/doctest.h>

#include <Alryn/Audio/Audio.h>
#include <Alryn/Audio/Sfx.h>

#include <cmath>

using namespace alryn;

TEST_CASE("every synthesized sfx clip renders a valid, audible, bounded buffer") {
    for (usize i = 0; i < kSfxCount; ++i) {
        const auto id = static_cast<SfxId>(i);
        const std::vector<f32> clip = render_sfx(id);
        CAPTURE(i);
        REQUIRE_FALSE(clip.empty());
        f32 peak = 0.0f;
        for (const f32 s : clip) {
            REQUIRE(std::isfinite(s));
            peak = std::max(peak, std::abs(s));
        }
        CHECK(peak > 0.05f); // actually audible...
        CHECK(peak <= 1.0f); // ...but never clipping the DAC
        // No start click: the first millisecond stays quiet (each voice has an attack envelope).
        f32 head = 0.0f;
        for (usize s = 0; s < std::min<usize>(clip.size(), kSfxSampleRate / 2000); ++s) {
            head = std::max(head, std::abs(clip[s]));
        }
        CHECK(head < 0.7f);
    }
}

TEST_CASE("the synth is deterministic and each clip has its own voice") {
    // Same id -> bit-identical render (deterministic noise/oscillators, no wall-clock state).
    const auto a1 = render_sfx(SfxId::SwordSwing);
    const auto a2 = render_sfx(SfxId::SwordSwing);
    REQUIRE(a1.size() == a2.size());
    CHECK(std::equal(a1.begin(), a1.end(), a2.begin()));
    // Different ids differ (in length or content) - the bank isn't one clip re-labelled.
    const auto coin = render_sfx(SfxId::Coin);
    const auto boom = render_sfx(SfxId::Explosion);
    CHECK(coin.size() != boom.size());
    // A one-shot UI tick is short; thunder rolls long.
    CHECK(render_sfx(SfxId::UiClick).size() < kSfxSampleRate / 4);
    CHECK(render_sfx(SfxId::Thunder).size() > kSfxSampleRate);
}

TEST_CASE("3D spatialisation pans by bearing and attenuates by distance") {
    const Vec3 listener{0.0f};
    const f32 facing_px = 0.0f; // facing +x
    // A source to the listener's RIGHT (facing +x, right = +z) favours the right ear.
    const StereoGain right = spatialize(listener, facing_px, Vec3{0.0f, 0.0f, 6.0f});
    CHECK(right.right > right.left);
    // ...and mirrored on the left.
    const StereoGain left = spatialize(listener, facing_px, Vec3{0.0f, 0.0f, -6.0f});
    CHECK(left.left > left.right);
    // Dead ahead is centred.
    const StereoGain ahead = spatialize(listener, facing_px, Vec3{6.0f, 0.0f, 0.0f});
    CHECK(ahead.left == doctest::Approx(ahead.right).epsilon(0.01));
    // Farther is quieter; past the cutoff it is culled to silence.
    const StereoGain near_g = spatialize(listener, facing_px, Vec3{4.0f, 0.0f, 0.0f});
    const StereoGain far_g = spatialize(listener, facing_px, Vec3{30.0f, 0.0f, 0.0f});
    CHECK(near_g.left + near_g.right > far_g.left + far_g.right);
    const StereoGain gone = spatialize(listener, facing_px, Vec3{kSfxMaxDistance + 5.0f, 0.0f, 0.0f});
    CHECK(gone.left == 0.0f);
    CHECK(gone.right == 0.0f);
    // Never fully one-eared: even hard right keeps some signal in the left ear.
    CHECK(right.left > 0.0f);
}

TEST_CASE("the audio subsystem starts gracefully with or without a sound device") {
    Audio audio;
    CHECK(audio.start()); // true even on a soundless box (then it just runs silent)
    if (!audio.ready()) {
        MESSAGE("no audio playback device - running the silent-fallback checks");
    }
    // The public API is safe in both modes (no device -> every call is a no-op).
    audio.set_master_volume(0.5f);
    CHECK(audio.master_volume() == doctest::Approx(0.5f));
    audio.set_listener(Vec3{1.0f, 2.0f, 3.0f}, 0.7f);
    audio.play(SfxId::Coin);
    audio.play_at(SfxId::SwordHit, Vec3{5.0f, 0.0f, 0.0f});
    audio.stop();
    audio.stop(); // idempotent
}
