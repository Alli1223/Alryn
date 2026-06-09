#pragma once

#include <Alryn/Core/Log.h>

#include <cstdlib>
#include <format>
#include <string_view>

namespace alryn::detail {

[[noreturn]] inline void assert_fail(const char* expr, const char* file, int line,
                                     std::string_view message) {
    Log::write(LogLevel::Fatal,
               std::format("Assertion failed: ({}) at {}:{}{}{}", expr, file, line,
                           message.empty() ? "" : " - ", message));
    std::abort();
}

} // namespace alryn::detail

// ALRYN_ASSERT: programmer-error checks, compiled out unless ALRYN_ENABLE_ASSERTS.
// ALRYN_VERIFY: invariant checks that stay active in every build.
// Both accept an optional std::format message: ALRYN_ASSERT(x > 0, "x was {}", x).
#define ALRYN_INTERNAL_CHECK(cond, ...)                                                  \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            ::alryn::detail::assert_fail(#cond, __FILE__, __LINE__,                       \
                                         ::std::format("" __VA_ARGS__));                  \
        }                                                                                \
    } while (false)

#ifdef ALRYN_ENABLE_ASSERTS
    #define ALRYN_ASSERT(cond, ...) ALRYN_INTERNAL_CHECK(cond, __VA_ARGS__)
#else
    #define ALRYN_ASSERT(cond, ...) ((void)sizeof(cond))
#endif

#define ALRYN_VERIFY(cond, ...) ALRYN_INTERNAL_CHECK(cond, __VA_ARGS__)
