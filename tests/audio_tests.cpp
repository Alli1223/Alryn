#include <doctest/doctest.h>

#include <Alryn/Audio/Audio.h>
#include <Alryn/Audio/Sfx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace alryn;

namespace {
// The time (s) of a clip's loudest 10 ms window.
f32 loudest_at(const std::vector<f32>& clip) {
    const usize win = kSfxSampleRate / 100;
    f32 best = 0.0f;
    usize best_i = 0;
    for (usize i = 0; i + win <= clip.size(); i += win / 2) {
        f32 e = 0.0f;
        for (usize s = i; s < i + win; ++s) {
            e += clip[s] * clip[s];
        }
        if (e > best) {
            best = e;
            best_i = i;
        }
    }
    return (static_cast<f32>(best_i) + static_cast<f32>(win) * 0.5f) / static_cast<f32>(kSfxSampleRate);
}

// 16-bit mono WAV, for auditioning the bank outside the game.
void write_wav(const std::string& path, const std::vector<f32>& clip) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return;
    }
    const u32 data_bytes = static_cast<u32>(clip.size() * 2);
    auto u32le = [&](u32 v) { std::fwrite(&v, 4, 1, f); };
    auto u16le = [&](u16 v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32le(36 + data_bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32le(16);
    u16le(1);
    u16le(1);
    u32le(kSfxSampleRate);
    u32le(kSfxSampleRate * 2);
    u16le(2);
    u16le(16);
    std::fwrite("data", 1, 4, f);
    u32le(data_bytes);
    for (const f32 s : clip) {
        const auto q = static_cast<i16>(std::clamp(s, -1.0f, 1.0f) * 32767.0f);
        std::fwrite(&q, 2, 1, f);
    }
    std::fclose(f);
}
} // namespace

TEST_CASE("every synthesized sfx clip renders a valid, audible, bounded buffer") {
    for (usize i = 0; i < kSfxCount; ++i) {
        const auto id = static_cast<SfxId>(i);
        REQUIRE(sfx_variants(id) >= 1);
        for (u32 v = 0; v < sfx_variants(id); ++v) {
            const std::vector<f32> clip = render_sfx(id, v);
            CAPTURE(i);
            CAPTURE(v);
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
            // No stop click either: every clip fades to silence.
            CHECK(std::abs(clip.back()) < 0.01f);
        }
    }
}

TEST_CASE("the sword sounds are timed to the swing animation") {
    // The whoosh plays on the click; its rush peaks while the blade cuts through (the forehand's cut
    // runs ~0.17-0.33 s, the blow lands at 0.27 s) - not at the start like a generic swish.
    for (u32 v = 0; v < sfx_variants(SfxId::SwordSwing); ++v) {
        CAPTURE(v);
        const f32 at = loudest_at(render_sfx(SfxId::SwordSwing, v));
        CHECK(at > 0.17f);
        CHECK(at < 0.3f);
    }
    // A landed blow is all attack: its loudest moment is right at the start.
    CHECK(loudest_at(render_sfx(SfxId::SwordHit)) < 0.03f);
    // The takes of a clip really differ (so repeats don't machine-gun one sample).
    const auto h0 = render_sfx(SfxId::SwordHit, 0);
    const auto h1 = render_sfx(SfxId::SwordHit, 1);
    CHECK_FALSE((h0.size() == h1.size() && std::equal(h0.begin(), h0.end(), h1.begin())));
}

// Set ALRYN_SFX_DUMP=<dir> to write every take of every clip there as 16-bit WAVs (to audition them).
TEST_CASE("sfx bank: optional WAV dump") {
    const char* dir = std::getenv("ALRYN_SFX_DUMP");
    if (dir == nullptr || *dir == 0) {
        return;
    }
    for (usize i = 0; i < kSfxCount; ++i) {
        const auto id = static_cast<SfxId>(i);
        for (u32 v = 0; v < sfx_variants(id); ++v) {
            write_wav(std::string{dir} + "/sfx" + std::to_string(i) + "_" + std::to_string(v) + ".wav", render_sfx(id, v));
        }
    }
    MESSAGE("wrote the sfx bank to " << std::string{dir});
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
