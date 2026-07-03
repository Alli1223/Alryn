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
constexpr usize kMaxVoices = 24; // a busy fight steals the oldest voice rather than growing
}

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

    std::array<std::vector<f32>, kSfxCount> bank; // rendered once in start()
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
    }

    void add_voice(SfxId id, f32 gain_l, f32 gain_r, f32 pitch) {
        if (!device_ready || (gain_l <= 0.0f && gain_r <= 0.0f)) {
            return; // silent no-op without a device, or a fully-attenuated 3D sound
        }
        const std::vector<f32>& clip = bank[static_cast<usize>(id)];
        if (clip.empty()) {
            return;
        }
        std::scoped_lock lock{mutex};
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
        impl_->bank[i] = render_sfx(static_cast<SfxId>(i));
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
