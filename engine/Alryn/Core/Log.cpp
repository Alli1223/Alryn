#include <Alryn/Core/Log.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <print>

#if defined(_WIN32)
#    include <io.h> // _isatty / _fileno
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h> // console virtual-terminal mode
#else
#    include <unistd.h> // isatty / fileno
#endif

namespace alryn {

LogLevel Log::s_level = LogLevel::Trace;

namespace {

std::mutex g_mutex;
bool g_color = false;

struct Style {
    std::string_view name;
    std::string_view color;
};

constexpr Style style_for(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return {"TRACE", "\x1b[37m"};   // white
        case LogLevel::Debug: return {"DEBUG", "\x1b[36m"};   // cyan
        case LogLevel::Info:  return {"INFO ", "\x1b[32m"};   // green
        case LogLevel::Warn:  return {"WARN ", "\x1b[33m"};   // yellow
        case LogLevel::Error: return {"ERROR", "\x1b[31m"};   // red
        case LogLevel::Fatal: return {"FATAL", "\x1b[1;41m"}; // white on red
        case LogLevel::Off:   return {"OFF  ", ""};
    }
    return {"?????", ""};
}

double seconds_since_start() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

void Log::init(LogLevel level) {
    s_level = level;
#if defined(_WIN32)
    g_color = ::_isatty(::_fileno(stderr)) != 0;
    if (g_color) {
        // Opt the console into ANSI escape processing so our color codes render
        // instead of printing literally (Windows 10+ / Terminal).
        const HANDLE h = ::GetStdHandle(STD_ERROR_HANDLE);
        DWORD mode = 0;
        if (h != INVALID_HANDLE_VALUE && ::GetConsoleMode(h, &mode) != 0) {
            ::SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
#else
    g_color = ::isatty(::fileno(stderr)) != 0;
#endif
    (void)seconds_since_start(); // anchor the start time
}

void Log::set_level(LogLevel level) noexcept {
    s_level = level;
}

LogLevel Log::level() noexcept {
    return s_level;
}

void Log::write(LogLevel level, std::string_view message) {
    const Style style = style_for(level);
    const std::scoped_lock lock(g_mutex);

    if (g_color) {
        std::println(stderr, "\x1b[90m[{:8.3f}]\x1b[0m {}[{}]\x1b[0m {}",
                     seconds_since_start(), style.color, style.name, message);
    } else {
        std::println(stderr, "[{:8.3f}] [{}] {}", seconds_since_start(), style.name, message);
    }
}

} // namespace alryn
