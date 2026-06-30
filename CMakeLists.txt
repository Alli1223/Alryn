cmake_minimum_required(VERSION 3.24)

project(Alryn
    VERSION 0.1.0
    DESCRIPTION "Alryn - a low-poly, deformable-terrain game engine (Vulkan, C++23)"
    LANGUAGES C CXX)

# --------------------------------------------------------------------------
#  Global settings
# --------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

# All runtime binaries land in build/bin so they share a working directory
# (this is where compiled shaders & assets are staged next to the executables).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# --------------------------------------------------------------------------
#  Options
# --------------------------------------------------------------------------
option(ALRYN_BUILD_TESTS       "Build Alryn unit/integration tests" ON)
option(ALRYN_BUILD_GAME        "Build the sample game"              ON)
option(ALRYN_ENABLE_VALIDATION "Enable Vulkan validation in Debug"  ON)
option(ALRYN_WARNINGS_AS_ERRORS "Treat warnings as errors (engine)" OFF)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(AlrynWarnings)
include(CompileShaders)

# --------------------------------------------------------------------------
#  Dependencies
#    System-first (Vulkan, GLM, GLFW), with FetchContent fallbacks so the
#    project builds on a clean machine with network access too.
# --------------------------------------------------------------------------
include(FetchContent)

# Vulkan: prefer a full system SDK. This machine has the loader + tools but not
# the C headers (no vulkan-headers package), so fall back to fetching the headers
# while still linking the installed loader. Either way we end up with a usable
# Vulkan::Vulkan target.
find_package(Vulkan QUIET)
if(NOT Vulkan_INCLUDE_DIR)
    message(STATUS "Vulkan headers not found on system - fetching Vulkan-Headers (loader is system: ${Vulkan_LIBRARY})")
    FetchContent_Declare(VulkanHeaders
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG v1.4.350)
    FetchContent_MakeAvailable(VulkanHeaders)

    find_library(Vulkan_LOADER_LIBRARY NAMES vulkan vulkan-1 REQUIRED)
    if(NOT TARGET Vulkan::Vulkan)
        add_library(Vulkan::Vulkan INTERFACE IMPORTED)
        target_link_libraries(Vulkan::Vulkan INTERFACE Vulkan::Headers ${Vulkan_LOADER_LIBRARY})
    endif()
    set(Vulkan_FOUND TRUE)
    set(Vulkan_VERSION "1.4.350 (fetched headers)")
endif()

# GLM (header-only maths) -- prefer the installed CMake config.
find_package(glm CONFIG QUIET)
if(NOT glm_FOUND AND NOT TARGET glm::glm)
    message(STATUS "glm not found on system - fetching")
    FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1)
    FetchContent_MakeAvailable(glm)
endif()

# GLFW (windowing/input) -- system config, then pkg-config, then source.
find_package(glfw3 CONFIG QUIET)
if(NOT TARGET glfw)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GLFW QUIET glfw3)
    endif()
    if(GLFW_FOUND)
        message(STATUS "glfw found via pkg-config")
        add_library(glfw INTERFACE)
        target_include_directories(glfw INTERFACE ${GLFW_INCLUDE_DIRS})
        target_link_libraries(glfw INTERFACE ${GLFW_LINK_LIBRARIES})
        target_link_directories(glfw INTERFACE ${GLFW_LIBRARY_DIRS})
    else()
        message(STATUS "glfw not found on system - fetching")
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
        FetchContent_MakeAvailable(glfw)
    endif()
endif()

# ENet (reliable/unreliable UDP). Vendored: we compile the sources ourselves so
# CMake 4 never sees ENet's ancient cmake_minimum_required. SOURCE_SUBDIR points at
# an existing folder with no CMakeLists.txt, so MakeAvailable downloads without
# configuring it.
FetchContent_Declare(enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG v1.3.18
    SOURCE_SUBDIR include)
FetchContent_MakeAvailable(enet)

set(alryn_enet_sources callbacks.c compress.c host.c list.c packet.c peer.c protocol.c)
if(WIN32)
    list(APPEND alryn_enet_sources win32.c)
else()
    list(APPEND alryn_enet_sources unix.c)
endif()
list(TRANSFORM alryn_enet_sources PREPEND ${enet_SOURCE_DIR}/)
add_library(enet STATIC ${alryn_enet_sources})
target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)
if(NOT WIN32)
    target_compile_definitions(enet PRIVATE
        HAS_SOCKLEN_T=1 HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1
        HAS_GETHOSTBYNAME_R=1 HAS_GETHOSTBYADDR_R=1 HAS_INET_PTON=1 HAS_INET_NTOP=1
        HAS_MSGHDR_FLAGS=1)
endif()

if(ALRYN_BUILD_TESTS)
    # doctest is header-only. We intentionally do NOT build its CMake project
    # (its cmake_minimum_required predates what CMake 4 accepts). Pointing
    # SOURCE_SUBDIR at a folder that has no CMakeLists.txt downloads the sources
    # without adding them to the build; we then expose the header ourselves.
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
        SOURCE_SUBDIR doctest)
    FetchContent_MakeAvailable(doctest)
    add_library(doctest_header INTERFACE)
    target_include_directories(doctest_header INTERFACE ${doctest_SOURCE_DIR})
    add_library(doctest::doctest ALIAS doctest_header)
    enable_testing()
endif()

# --------------------------------------------------------------------------
#  Targets
# --------------------------------------------------------------------------
add_subdirectory(engine)

# Compile GLSL -> SPIR-V (staged into build/bin/shaders next to the binaries).
alryn_compile_shaders(alryn SHADERS
    ${CMAKE_SOURCE_DIR}/shaders/mesh.vert
    ${CMAKE_SOURCE_DIR}/shaders/mesh.frag
    ${CMAKE_SOURCE_DIR}/shaders/foliage.frag
    ${CMAKE_SOURCE_DIR}/shaders/water.vert
    ${CMAKE_SOURCE_DIR}/shaders/water.frag
    ${CMAKE_SOURCE_DIR}/shaders/shadow.vert
    ${CMAKE_SOURCE_DIR}/shaders/emissive.frag
    ${CMAKE_SOURCE_DIR}/shaders/grass.vert
    ${CMAKE_SOURCE_DIR}/shaders/ui.vert
    ${CMAKE_SOURCE_DIR}/shaders/ui.frag
    ${CMAKE_SOURCE_DIR}/shaders/sky.vert
    ${CMAKE_SOURCE_DIR}/shaders/sky.frag)

if(ALRYN_BUILD_GAME)
    add_subdirectory(game)
endif()
if(ALRYN_BUILD_TESTS)
    add_subdirectory(tests)
endif()
# Art-iteration tools (asset_preview); needs the renderer stack, so it follows the game flag.
if(ALRYN_BUILD_GAME)
    add_subdirectory(tools)
endif()

# --------------------------------------------------------------------------
#  Configuration summary
# --------------------------------------------------------------------------
message(STATUS "")
message(STATUS "==== Alryn ${PROJECT_VERSION} =========================")
message(STATUS "  Build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard   : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler       : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Vulkan         : ${Vulkan_VERSION}")
message(STATUS "  Build tests    : ${ALRYN_BUILD_TESTS}")
message(STATUS "  Build game     : ${ALRYN_BUILD_GAME}")
message(STATUS "============================================")
message(STATUS "")
