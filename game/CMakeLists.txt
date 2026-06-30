# The sample game built on top of the engine. For now it is a headless demo that
# exercises the Application/Scene/Component foundation; later milestones turn it
# into a windowed client plus a dedicated server.
add_executable(alryn_game
    main.cpp
    Bot.cpp
    ClientApp.cpp
    ClientAppMenu.cpp
    ClientAppInput.cpp
    ClientAppCharacter.cpp
    ClientAppCloth.cpp
    ClientAppWorld.cpp
    ClientAppWagons.cpp
    ClientAppVfx.cpp
    ClientAppHud.cpp
)

target_link_libraries(alryn_game PRIVATE alryn)

alryn_set_warnings(alryn_game)
