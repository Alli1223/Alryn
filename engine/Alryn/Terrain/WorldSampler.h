#pragma once

#include <Alryn/Core/Density.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Terrain/WorldGen.h>

#include <vector>

namespace alryn {

struct WorldEdit {
    Vec3 center{0.0f};
    f32 radius = 0.0f;
    f32 amount = 0.0f;
};

// The authoritative density of the world: deterministic base terrain (from a
// seed) plus a list of replicated sphere edits. Callable as a DensitySampler.
// Cheap to copy the seed; edits are shared by reference where it matters.
class WorldSampler {
public:
    WorldSampler() = default;
    explicit WorldSampler(u32 seed) : seed_(seed) {}

    void set_seed(u32 seed) { seed_ = seed; }
    u32 seed() const { return seed_; }

    void add_edit(const Vec3& center, f32 radius, f32 amount) {
        edits_.push_back({center, radius, amount});
    }
    const std::vector<WorldEdit>& edits() const { return edits_; }

    f32 operator()(const Vec3& p) const {
        f32 value = worldgen::density(p, seed_);
        for (const WorldEdit& e : edits_) {
            const f32 d = glm::length(p - e.center);
            if (d < e.radius) {
                value += e.amount * (1.0f - d / e.radius);
            }
        }
        return glm::clamp(value, -1.0f, 1.0f);
    }

    // Convenience: bind this sampler into a DensitySampler function.
    DensitySampler as_sampler() const {
        return [this](const Vec3& p) { return (*this)(p); };
    }

private:
    u32 seed_ = 0;
    std::vector<WorldEdit> edits_;
};

} // namespace alryn
