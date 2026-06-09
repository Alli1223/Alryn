#pragma once

#include <Alryn/Core/Types.h>

#include <format>
#include <string_view>
#include <utility>

namespace alryn {

enum class LogLevel : u8 {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off,
};

// Tiny, dependency-free logger. Thread-safe writes, monotonic timestamps, and
// ANSI colour when the output is a terminal. Formatting uses std::format.
class Log {
public:
    static void init(LogLevel level = LogLevel::Trace);
    static void set_level(LogLevel level) noexcept;
    static LogLevel level() noexcept;

    // Writes an already-formatted message (used by the macros and by Assert).
    static void write(LogLevel level, std::string_view message);

    template <typename... Args>
    static void log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
        if (s_level == LogLevel::Off || level < s_level) {
            return;
        }
        write(level, std::format(fmt, std::forward<Args>(args)...));
    }

private:
    static LogLevel s_level;
};

} // namespace alryn

#define ALRYN_TRACE(...) ::alryn::Log::log(::alryn::LogLevel::Trace, __VA_ARGS__)
#define ALRYN_DEBUG(...) ::alryn::Log::log(::alryn::LogLevel::Debug, __VA_ARGS__)
#define ALRYN_INFO(...)  ::alryn::Log::log(::alryn::LogLevel::Info, __VA_ARGS__)
#define ALRYN_WARN(...)  ::alryn::Log::log(::alryn::LogLevel::Warn, __VA_ARGS__)
#define ALRYN_ERROR(...) ::alryn::Log::log(::alryn::LogLevel::Error, __VA_ARGS__)
#define ALRYN_FATAL(...) ::alryn::Log::log(::alryn::LogLevel::Fatal, __VA_ARGS__)
