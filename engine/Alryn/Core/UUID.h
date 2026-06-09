#pragma once

#include <Alryn/Core/Types.h>

#include <functional>

namespace alryn {

// 64-bit random identifier for engine objects. Cheap to copy/compare/hash.
class UUID {
public:
    UUID(); // random, non-zero
    constexpr explicit UUID(u64 value) : value_(value) {}

    constexpr u64 value() const { return value_; }
    constexpr explicit operator u64() const { return value_; }
    constexpr bool valid() const { return value_ != 0; }

    friend constexpr bool operator==(const UUID&, const UUID&) = default;

private:
    u64 value_ = 0;
};

} // namespace alryn

template <>
struct std::hash<alryn::UUID> {
    std::size_t operator()(const alryn::UUID& id) const noexcept {
        return std::hash<alryn::u64>{}(id.value());
    }
};
