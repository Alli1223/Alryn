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
}

void GameManager::update(Timestep dt) {
    time_of_day_ += dt.seconds / day_seconds_;
    time_of_day_ -= std::floor(time_of_day_);
}

} // namespace alryn
