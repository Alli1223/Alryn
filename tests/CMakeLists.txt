add_executable(alryn_tests
    test_main.cpp
    core_tests.cpp
    scene_tests.cpp
    platform_tests.cpp
    renderer_tests.cpp
    mesh_tests.cpp
    render_offscreen_tests.cpp
    terrain_tests.cpp
    physics_tests.cpp
    net_tests.cpp
    character_tests.cpp
    ui_tests.cpp
)

target_link_libraries(alryn_tests PRIVATE
    alryn
    doctest::doctest)

add_test(NAME alryn_tests COMMAND alryn_tests)
