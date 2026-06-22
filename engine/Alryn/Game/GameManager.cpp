#include <Alryn/Game/GameManager.h>

#include <Alryn/Core/Math.h> // glm::clamp

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace alryn {

void GameManager::init() {
    if (const char* t = std::getenv("ALRYN_TIME")) {
        time_of_day_ = glm::clamp(static_cast<f32>(std::atof(t)), 0.0f, 1.0f);
    }
    if (const char* d = std::getenv("ALRYN_DAY_SECONDS")) {
        day_seconds_ = std::max(5.0f, static_cast<f32>(std::atof(d)));
    }
    if (const char* wv = std::getenv("ALRYN_WEATHER")) {
        weather_force_ = glm::clamp(static_cast<f32>(std::atof(wv)), 0.0f, 1.0f);
        weather_ = weather_force_;
    }
}

void GameManager::update(Timestep dt) {
    time_of_day_ += dt.seconds / day_seconds_;
    time_of_day_ -= std::floor(time_of_day_);

    // Weather: a slow layered-sine cycle biased toward clear, so storms build occasionally as you
    // travel rather than constantly. (ALRYN_WEATHER pins it for testing/preview.)
    if (weather_force_ >= 0.0f) {
        weather_ = weather_force_;
        return;
    }
    weather_clock_ += dt.seconds;
    const f32 t = weather_clock_;
    f32 w = std::sin(t * 0.013f) * 0.5f + std::sin(t * 0.031f + 1.7f) * 0.3f +
            std::sin(t * 0.0061f + 4.2f) * 0.2f;
    w = w * 0.5f + 0.5f;                         // 0..1
    weather_ = glm::smoothstep(0.52f, 0.92f, w); // clear most of the time, ramps into a storm
}

} // namespace alryn
