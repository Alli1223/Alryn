#include <Alryn/Core/UUID.h>

#include <limits>
#include <random>

namespace alryn {

namespace {

std::mt19937_64& engine() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    return rng;
}

} // namespace

UUID::UUID() {
    std::uniform_int_distribution<u64> dist(1, std::numeric_limits<u64>::max());
    value_ = dist(engine());
}

} // namespace alryn
