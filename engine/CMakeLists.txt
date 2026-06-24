# The engine is one static library, organised by module (Core, Engine, Scene,
# Platform, Renderer...). Public headers are reached via the include root so
# game/test code writes  #include <Alryn/Core/Log.h>.

add_library(alryn STATIC
    # ---- Core --------------------------------------------------------------
    Alryn/Core/Log.cpp
    Alryn/Core/UUID.cpp
    Alryn/Core/Paths.cpp
    Alryn/Core/Noise.cpp
    Alryn/Core/Density.cpp
    # ---- Engine ------------------------------------------------------------
    Alryn/Engine/Engine.cpp
    Alryn/Engine/Application.cpp
    # ---- Scene -------------------------------------------------------------
    Alryn/Scene/Transform.cpp
    Alryn/Scene/GameObject.cpp
    Alryn/Scene/Scene.cpp
    Alryn/Scene/Camera.cpp
    # ---- Platform ----------------------------------------------------------
    Alryn/Platform/Window.cpp
    Alryn/Platform/Input.cpp
    # ---- Physics -----------------------------------------------------------
    Alryn/Physics/CharacterController.cpp
    Alryn/Physics/CollisionWorld.cpp
    # ---- Character ---------------------------------------------------------
    Alryn/Character/CharacterModel.cpp
    Alryn/Character/CharacterAnimator.cpp
    Alryn/Character/Outfit.cpp
    # ---- Renderer (Vulkan RHI) ---------------------------------------------
    Alryn/Renderer/Vulkan/VulkanCommon.cpp
    Alryn/Renderer/Vulkan/VulkanInstance.cpp
    Alryn/Renderer/Vulkan/VulkanDevice.cpp
    Alryn/Renderer/Vulkan/VulkanBuffer.cpp
    Alryn/Renderer/Vulkan/VulkanImage.cpp
    Alryn/Renderer/Vulkan/VulkanPipeline.cpp
    Alryn/Renderer/Vulkan/VulkanSwapchain.cpp
    Alryn/Renderer/Mesh.cpp
    Alryn/Renderer/MeshPrimitives.cpp
    Alryn/Renderer/Renderer.cpp
    # ---- UI ----------------------------------------------------------------
    Alryn/UI/VectorFont.cpp
    Alryn/UI/Widget.cpp
    Alryn/UI/Widgets.cpp
    Alryn/UI/DrawList.cpp
    Alryn/UI/UIContext.cpp
    # ---- Terrain -----------------------------------------------------------
    Alryn/Terrain/VoxelField.cpp
    Alryn/Terrain/MarchingTetra.cpp
    Alryn/Terrain/Terrain.cpp
    Alryn/Terrain/StreamingTerrain.cpp
    Alryn/Terrain/RoadNetwork.cpp
    # ---- World -------------------------------------------------------------
    Alryn/World/PropLibrary.cpp
    Alryn/World/VehicleTypes.cpp
    # ---- Game --------------------------------------------------------------
    Alryn/Game/GameManager.cpp
    Alryn/Game/Contracts.cpp
    Alryn/Game/Abilities.cpp
    # ---- Combat ------------------------------------------------------------
    Alryn/Combat/SiegeMode.cpp
    # ---- Net ---------------------------------------------------------------
    Alryn/Net/ByteBuffer.cpp
    Alryn/Net/Protocol.cpp
    Alryn/Net/NetCommon.cpp
    Alryn/Net/NetServer.cpp
    Alryn/Net/NetClient.cpp
    Alryn/Net/GameServer.cpp
)

add_library(Alryn::alryn ALIAS alryn)

target_include_directories(alryn PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

target_compile_features(alryn PUBLIC cxx_std_23)

target_compile_definitions(alryn PUBLIC
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
    GLM_FORCE_SILENT_WARNINGS
    $<$<CONFIG:Debug>:ALRYN_BUILD_DEBUG ALRYN_ENABLE_ASSERTS>)

target_link_libraries(alryn
    PUBLIC
        glm::glm
        Vulkan::Vulkan
    PRIVATE
        glfw
        enet)

alryn_set_warnings(alryn)
