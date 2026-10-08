#pragma once

#include <Alryn/Core/Density.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Terrain/WorldGen.h>

#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

namespace alryn {

struct WorldEdit {
    Vec3 center{0.0f};
    f32 radius = 0.0f;
    f32 amount = 0.0f;
};

// The runtime terrain edits (digs, raised earth, craters) with a coarse xz grid over them, so a
// density sample only visits the handful of edits near it instead of every edit ever made - a long
// session of craters and spade work would otherwise slow EVERY collision / raycast / chunk fill.
class WorldEdits {
public:
    static constexpr f32 kCell = 8.0f; // grid cell (metres); an edit is listed in every cell it overlaps

    void add(const WorldEdit& e) {
        const u32 index = static_cast<u32>(list_.size());
        list_.push_back(e);
        const int x0 = cell(e.center.x - e.radius), x1 = cell(e.center.x + e.radius);
        const int z0 = cell(e.center.z - e.radius), z1 = cell(e.center.z + e.radius);
        for (int cz = z0; cz <= z1; ++cz) {
            for (int cx = x0; cx <= x1; ++cx) {
                grid_[key(cx, cz)].push_back(index);
            }
        }
    }
    const std::vector<WorldEdit>& list() const { return list_; }
    bool empty() const { return list_.empty(); }

    // The summed edit contribution at `p` (before the density clamp).
    f32 sum(const Vec3& p) const {
        if (list_.empty()) {
            return 0.0f;
        }
        const auto it = grid_.find(key(cell(p.x), cell(p.z)));
        if (it == grid_.end()) {
            return 0.0f;
        }
        f32 value = 0.0f;
        for (const u32 i : it->second) {
            const WorldEdit& e = list_[i];
            const f32 d = glm::length(p - e.center);
            if (d < e.radius) {
                value += e.amount * (1.0f - d / e.radius);
            }
        }
        return value;
    }

    // The edits whose sphere reaches into the xz box [lo, hi] (e.g. one terrain chunk), in order.
    std::vector<WorldEdit> overlapping(const Vec2& lo, const Vec2& hi) const {
        std::vector<WorldEdit> out;
        for (const WorldEdit& e : list_) {
            if (e.center.x + e.radius >= lo.x && e.center.x - e.radius <= hi.x && e.center.z + e.radius >= lo.y &&
                e.center.z - e.radius <= hi.y) {
                out.push_back(e);
            }
        }
        return out;
    }

private:
    static int cell(f32 v) { return static_cast<int>(std::floor(v / kCell)); }
    static i64 key(int cx, int cz) { return (static_cast<i64>(cx) << 32) | static_cast<i64>(static_cast<u32>(cz)); }

    std::vector<WorldEdit> list_;
    std::unordered_map<i64, std::vector<u32>> grid_;
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

    void add_edit(const Vec3& center, f32 radius, f32 amount) { edits_.add({center, radius, amount}); }
    const std::vector<WorldEdit>& edits() const { return edits_.list(); }
    const WorldEdits& edit_index() const { return edits_; }

    f32 operator()(const Vec3& p) const {
        return glm::clamp(worldgen::density(p, seed_) + edits_.sum(p), -1.0f, 1.0f);
    }

    // Convenience: bind this sampler into a DensitySampler function (captures `this`,
    // so only use it while this WorldSampler outlives the function).
    DensitySampler as_sampler() const {
        return [this](const Vec3& p) { return (*this)(p); };
    }

    // A self-contained DensitySampler that copies the seed + edits by value, so it is
    // safe to evaluate on another thread (used by the async terrain streamer).
    DensitySampler snapshot() const {
        auto edits = std::make_shared<const WorldEdits>(edits_);
        return [seed = seed_, edits](const Vec3& p) {
            return glm::clamp(worldgen::density(p, seed) + edits->sum(p), -1.0f, 1.0f);
        };
    }

private:
    u32 seed_ = 0;
    WorldEdits edits_;
};

} // namespace alryn
