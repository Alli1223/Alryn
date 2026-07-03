#include <Alryn/Core/Paths.h>

#include <array>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h> // GetModuleFileNameW
#else
#    include <unistd.h> // readlink
#endif

namespace alryn {

namespace fs = std::filesystem;

fs::path executable_dir() {
#if defined(_WIN32)
    std::array<wchar_t, 4096> buffer{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                              static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return fs::current_path();
    }
    return fs::path(buffer.data(), buffer.data() + length).parent_path();
#else
    std::array<char, 4096> buffer{};
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) {
        return fs::current_path();
    }
    buffer[static_cast<std::size_t>(length)] = '\0';
    return fs::path(buffer.data()).parent_path();
#endif
}

fs::path asset_path(std::string_view relative) {
    return executable_dir() / relative;
}

fs::path shader_path(std::string_view name) {
    return executable_dir() / "shaders" / name;
}

} // namespace alryn
