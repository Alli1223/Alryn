#pragma once

#include <Alryn/Audio/Sfx.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Engine/Subsystem.h>

#include <memory>

// The audio subsystem: a fire-and-forget mixer of the procedurally-synthesized SFX bank (Sfx.h)
// over a miniaudio playback device. Degrades gracefully - on a machine with no sound server
// (CI, a headless box) start() still succeeds and every play() is a silent no-op, mirroring how
// the GPU/display tests skip. The miniaudio types are hidden behind an Impl so no engine header
// ever includes the backend.
namespace alryn {

class Audio : public Subsystem {
public:
    Audio();
    ~Audio() override;

    const char* name() const override { return "Audio"; }
    bool on_init(Engine& engine) override {
        (void)engine;
        return start(); // always true - "no audio device" is a warning, not a startup failure
    }
    void on_shutdown() override { stop(); }

    // Open the playback device + render the clip bank. Safe to call directly (tests / tools);
    // returns true even when no device could be opened (the subsystem then runs silent).
    bool start();
    void stop();
    bool ready() const; // a real device is open and playing

    void set_master_volume(f32 v); // 0..1
    f32 master_volume() const;

    // Where the ears are: the local player/camera position + facing yaw, fed each frame so 3D
    // sounds attenuate with distance and pan across the stereo field (see spatialize in Sfx.h).
    void set_listener(const Vec3& position, f32 yaw);

    // Fire-and-forget playback. `pitch` scales playback speed (1 = as rendered); a little
    // variation keeps repeated hits from sounding machine-gunned.
    void play(SfxId id, f32 gain = 1.0f, f32 pitch = 1.0f);                       // 2D (UI/local)
    void play_at(SfxId id, const Vec3& world, f32 gain = 1.0f, f32 pitch = 1.0f); // 3D in-world

private:
    struct Impl; // miniaudio device + voice mixer (Audio.cpp)
    std::unique_ptr<Impl> impl_;
};

} // namespace alryn
