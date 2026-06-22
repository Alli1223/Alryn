# Standalone art-iteration tools. `asset_preview` renders a single generated asset in isolation
# (reusing the headless OffscreenRenderer from tests/support, so it matches in-game lighting).
add_executable(asset_preview
    asset_preview.cpp
    ${CMAKE_SOURCE_DIR}/tests/support/OffscreenRenderer.cpp)

target_include_directories(asset_preview PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(asset_preview PRIVATE alryn)
