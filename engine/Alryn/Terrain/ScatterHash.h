#pragma once

#include <Alryn/Core/Types.h>

namespace alryn::detail {

// Deterministic spatial hash used by every scatter pass (trees, props, houses,
// vegetation). Same (x,z,salt) always yields the same value, so chunks tile
// seamlessly and the client + server agree on placement.
inline u32 tree_hash(int x, int z, u32 salt) {
    u32 h = salt + 0x9E3779B9u + static_cast<u32>(x) * 0x85EBCA77u + static_cast<u32>(z) * 0xC2B2AE3Du;
    h = (h ^ (h >> 15)) * 0x2545F491u;
    h = (h ^ (h >> 13)) * 0x27D4EB2Fu;
    return h ^ (h >> 16);
}

inline f32 hash01(u32 h) {
    return static_cast<f32>(h & 0xFFFFFFu) / static_cast<f32>(0xFFFFFFu);
}

} // namespace alryn::detail
