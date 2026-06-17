#pragma once

#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>

namespace alryn {

// The server-side game-mode orchestrator. Today it owns the authoritative day/night
// clock (mirrored to clients in each Snapshot for lighting). It is the seam where the
// goods-transport objective - the cart, its source + destination towns, and the
// win/lose verdict - will live, and where the (currently dormant) night siege in
// Combat/SiegeMode.cpp would be re-attached.
class GameManager {
public:
    // Reads ALRYN_TIME (start time, 0..1) and ALRYN_DAY_SECONDS (cycle length).
    void init();

    // Advances the clock (and, later, drives the transport objective).
    void update(Timestep dt);

    f32 time_of_day() const { return time_of_day_; }

private:
    f32 time_of_day_ = 0.30f; // 0..1 day/night clock
    f32 day_seconds_ = 120.0f;
};

} // namespace alryn
