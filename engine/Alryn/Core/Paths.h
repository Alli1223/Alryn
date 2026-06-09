#pragma once

#include <filesystem>
#include <string_view>

namespace alryn {

// Directory containing the running executable (resolved from /proc/self/exe on
// Linux). Compiled shaders and assets are staged next to the binary, so paths
// resolve regardless of the working directory.
std::filesystem::path executable_dir();

// executable_dir() / relative
std::filesystem::path asset_path(std::string_view relative);

// executable_dir() / "shaders" / name   (e.g. shader_path("mesh.vert.spv"))
std::filesystem::path shader_path(std::string_view name);

} // namespace alryn
