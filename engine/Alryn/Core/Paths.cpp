#include <Alryn/Core/Paths.h>

#include <array>

#include <unistd.h> // readlink

namespace alryn {

namespace fs = std::filesystem;

fs::path executable_dir() {
    std::array<char, 4096> buffer{};
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) {
        return fs::current_path();
    }
    buffer[static_cast<std::size_t>(length)] = '\0';
    return fs::path(buffer.data()).parent_path();
}

fs::path asset_path(std::string_view relative) {
    return executable_dir() / relative;
}

fs::path shader_path(std::string_view name) {
    return executable_dir() / "shaders" / name;
}

} // namespace alryn
