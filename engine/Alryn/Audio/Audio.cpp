// Audio subsystem implementation: renders the whole SFX bank up front (pure synth, no assets),
// opens a miniaudio playback device, and mixes a small pool of fire-and-forget voices in the
// device's data callback. The voice list is guarded by a mutex - the critical sections are a few
// dozen instructions, far below the callback's real-time budget at 48 kHz.

#include <Alryn/Audio/Audio.h>
#include <Alryn/Core/Log.h>

#include <miniaudio.h>

#include <array>
#include <cmath>
#include <mutex>
#include <vector>

namespace alryn {

namespace {
constexpr usize kMaxVoices = 32; // a busy fight steals the oldest voice rather than growing

// The output's gentle limiter: transparent below 0.8, then a soft knee that never passes 1.0 - a
// pile-up of voices in a big fight thickens instead of crackling into hard clipping.
inline f32 soft_limit(f32 x) {
    const f32 a = std::abs(x);
    if (a <= 0.8f) {
        return x;
    }
    return std::copysign(0.8f + 0.2f * std::tanh((a - 0.8f) / 0.2f), x);
}
} // namespace

struct Audio::Impl {
    // A playing instance of a bank clip. `cursor` walks the mono clip at `pitch` samples/frame
    // (linear interpolation), mixed into the stereo output at the per-ear gains.
    struct Voice {
        const std::vector<f32>* clip = nullptr;
        f64 cursor = 0.0;
        f32 pitch = 1.0f;
        f32 gain_l = 0.0f;
        f32 gain_r = 0.0f;
    };

    std::array<std::vector<std::vector<f32>>, kSfxCount> bank; // every take of every clip, rendered once in start()
    std::array<u32, kSfxCount> last_take{};                     // the take each clip played last (never twice running)
    u32 rng = 0x2545f491u;
    std::mutex mutex;                             // guards voices + listener + master
    std::vector<Voice> voices;
    Vec3 listener{0.0f};
    f32 listener_yaw = 0.0f;
    f32 master = 0.8f;
    bool device_ready = false;
    ma_device device{};

    static void data_callback(ma_device* dev, void* out, const void* /*in*/, ma_uint32 frames) {
        auto* impl = static_cast<Impl*>(dev->pUserData);
        auto* dst = static_cast<f32*>(out); // interleaved stereo f32, pre-zeroed by miniaudio
        std::scoped_lock lock{impl->mutex};
        const f32 master = impl->master;
        for (Voice& v : impl->voices) {
            const std::vector<f32>& clip = *v.clip;
            for (ma_uint32 f = 0; f < frames; ++f) {
                const usize i = static_cast<usize>(v.cursor);
                if (i + 1 >= clip.size()) {
                    v.clip = nullptr; // spent - reaped below
                    break;
                }
                const f32 frac = static_cast<f32>(v.cursor - static_cast<f64>(i));
                const f32 s = clip[i] + (clip[i + 1] - clip[i]) * frac; // linear resample
                dst[f * 2 + 0] += s * v.gain_l * master;
                dst[f * 2 + 1] += s * v.gain_r * master;
                v.cursor += static_cast<f64>(v.pitch);
            }
        }
        std::erase_if(impl->voices, [](const Voice& v) { return v.clip == nullptr; });
        for (ma_uint32 s = 0; s < frames * 2; ++s) {
            dst[s] = soft_limit(dst[s]);
        }
    }

    void add_voice(SfxId id, f32 gain_l, f32 gain_r, f32 pitch) {
        if (!device_ready || (gain_l <= 0.0f && gain_r <= 0.0f)) {
            return; // silent no-op without a device, or a fully-attenuated 3D sound
        }
        const usize ci = static_cast<usize>(id);
        const std::vector<std::vector<f32>>& takes = bank[ci];
        if (takes.empty()) {
            return;
        }
        std::scoped_lock lock{mutex};
        // Rotate through the clip's takes at random, never the same one twice in a row.
        u32 take = 0;
        if (takes.size() > 1) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            take = rng % static_cast<u32>(takes.size() - 1);
            if (take >= last_take[ci]) {
                ++take;
            }
        }
        last_take[ci] = take;
        const std::vector<f32>& clip = takes[take];
        if (clip.empty()) {
            return;
        }
        if (voices.size() >= kMaxVoices) {
            voices.erase(voices.begin()); // steal the oldest
        }
        voices.push_back({&clip, 0.0, glm::clamp(pitch, 0.25f, 4.0f), gain_l, gain_r});
    }
};

Audio::Audio() : impl_(std::make_unique<Impl>()) {}

Audio::~Audio() {
    stop();
}

bool Audio::start() {
    if (impl_->device_ready) {
        return true;
    }
    for (usize i = 0; i < kSfxCount; ++i) { // synthesize the whole bank (no assets)
        const auto id = static_cast<SfxId>(i);
        impl_->bank[i].clear();
        for (u32 v = 0; v < sfx_variants(id); ++v) {
            impl_->bank[i].push_back(render_sfx(id, v));
        }
    }
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = kSfxSampleRate;
    config.dataCallback = Impl::data_callback;
    config.pUserData = impl_.get();
    if (ma_device_init(nullptr, &config, &impl_->device) != MA_SUCCESS ||
        ma_device_start(&impl_->device) != MA_SUCCESS) {
        ALRYN_WARN("Audio: no playback device available - running silent");
        return true; // graceful: the game runs, play() just no-ops
    }
    impl_->device_ready = true;
    ALRYN_INFO("Audio: playback device open ({} Hz, {} synthesized clips)", kSfxSampleRate,
               kSfxCount);
    return true;
}

void Audio::stop() {
    if (impl_->device_ready) {
        ma_device_uninit(&impl_->device); // stops the callback before returning
        impl_->device_ready = false;
    }
    std::scoped_lock lock{impl_->mutex};
    impl_->voices.clear();
}

bool Audio::ready() const {
    return impl_->device_ready;
}

void Audio::set_master_volume(f32 v) {
    std::scoped_lock lock{impl_->mutex};
    impl_->master = glm::clamp(v, 0.0f, 1.0f);
}

f32 Audio::master_volume() const {
    std::scoped_lock lock{impl_->mutex};
    return impl_->master;
}

void Audio::set_listener(const Vec3& position, f32 yaw) {
    std::scoped_lock lock{impl_->mutex};
    impl_->listener = position;
    impl_->listener_yaw = yaw;
}

void Audio::play(SfxId id, f32 gain, f32 pitch) {
    const f32 g = gain * 0.707f; // equal-power centre
    impl_->add_voice(id, g, g, pitch);
}

void Audio::play_at(SfxId id, const Vec3& world, f32 gain, f32 pitch) {
    Vec3 listener;
    f32 yaw;
    {
        std::scoped_lock lock{impl_->mutex};
        listener = impl_->listener;
        yaw = impl_->listener_yaw;
    }
    const StereoGain sg = spatialize(listener, yaw, world, gain);
    impl_->add_voice(id, sg.left, sg.right, pitch);
}

} // namespace alryn
