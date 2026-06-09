#pragma once

#include <Alryn/Core/Types.h>

#include <chrono>

namespace alryn {

// A frame delta-time. Implicitly converts to float seconds for convenience.
struct Timestep {
    f32 seconds = 0.0f;

    constexpr Timestep() = default;
    constexpr explicit Timestep(f32 s) : seconds(s) {}

    constexpr operator f32() const { return seconds; }
    constexpr f32 ms() const { return seconds * 1000.0f; }
};

// Monotonic stopwatch built on steady_clock.
class Clock {
public:
    Clock() : start_(now()), last_(start_) {}

    void reset() {
        start_ = last_ = now();
    }

    // Seconds since construction / last reset().
    f64 elapsed() const {
        return std::chrono::duration<f64>(now() - start_).count();
    }

    // Seconds since the previous restart() (or construction), then re-arms.
    f64 restart() {
        const auto t = now();
        const f64 delta = std::chrono::duration<f64>(t - last_).count();
        last_ = t;
        return delta;
    }

private:
    using SteadyClock = std::chrono::steady_clock;
    static SteadyClock::time_point now() { return SteadyClock::now(); }

    SteadyClock::time_point start_;
    SteadyClock::time_point last_;
};

} // namespace alryn
