# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

# The Vulkan backend TUs all parse vulkan.hpp (via Veng/Renderer/Backend/Vulkan.h),
# which dominates their compile time and which the library-wide Veng.h PCH cannot
# cover without taxing every non-backend TU. They build as their own object library
# so a second PCH precompiles the Vulkan header for exactly these TUs; the objects
# are absorbed into veng below via $<TARGET_OBJECTS>, so nothing links this target.
add_library(veng_backend_objs OBJECT
        src/Renderer/Backend/Barrier.cpp
        src/Renderer/Backend/BarrierDecision.cpp
        src/Renderer/Backend/RenderGraphSchedule.cpp
        src/Renderer/Backend/BindlessRegistry.cpp
        src/Renderer/Backend/TransientAllocation.cpp
        src/Renderer/Backend/Buffer.cpp
        src/Renderer/Backend/CommandBuffer.cpp
        src/Renderer/Backend/CommandPool.cpp
        src/Renderer/Backend/ComputePipeline.cpp
        src/Renderer/Backend/Context.cpp
        src/Renderer/Backend/DebugMarkers.cpp
        src/Renderer/Backend/DescriptorPool.cpp
        src/Renderer/Backend/DescriptorSet.cpp
        src/Renderer/Backend/DescriptorSetLayout.cpp
        src/Renderer/Backend/Fence.cpp
        src/Renderer/Backend/GraphicsPipeline.cpp
        src/Renderer/Backend/Image.cpp
        src/Renderer/Backend/ImageView.cpp
        src/Renderer/Backend/PipelineLayout.cpp
        src/Renderer/Backend/Sampler.cpp
        src/Renderer/Backend/Semaphore.cpp
        src/Renderer/Backend/ShaderModule.cpp
        src/Renderer/Backend/SwapChain.cpp
        src/Renderer/Backend/SynchronizationFrame.cpp
        src/Renderer/Backend/TimelineSemaphore.cpp
        src/Renderer/Backend/Utils.cpp
        src/Renderer/Backend/VertexBufferLayout.cpp
)
# The objects land inside the veng shared library, so they must be PIC, carry the
# VE_API export annotations' build-side define, and compile under the same
# no-exceptions contract as the rest of the library.
set_target_properties(veng_backend_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(veng_backend_objs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(veng_backend_objs PRIVATE VE_BUILD_SHARED)
target_compile_options(veng_backend_objs PRIVATE -fno-exceptions)
if (VE_DEBUG)
    target_compile_definitions(veng_backend_objs PRIVATE VE_ENABLE_VALIDATION_LAYERS=1 VE_DEBUG=1)
endif ()
# See the matching veng block below: the dynamic-dispatcher storage lives in
# Context.cpp, which compiles here.
if (WIN32)
    target_compile_definitions(veng_backend_objs PRIVATE
            VULKAN_HPP_STORAGE_SHARED VULKAN_HPP_STORAGE_SHARED_EXPORT)
endif ()
# Compile-time usage requirements only — the object library is never installed or
# exported, so these links stay out of the SDK export set.
target_link_libraries(veng_backend_objs PRIVATE
        glm::glm
        fmt::fmt
        veng::assetpack
        Vulkan::Vulkan
        glfw
        GPUOpen::VulkanMemoryAllocator
)
target_precompile_headers(veng_backend_objs PRIVATE
        <Veng/Veng.h> <Veng/Renderer/Backend/Vulkan.h>)
# Context.cpp defines VMA_IMPLEMENTATION ahead of its includes; a force-included
# PCH would have already pulled the VMA header without it, so it opts out.
set_source_files_properties(src/Renderer/Backend/Context.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

add_library(veng SHARED
        src/Application.cpp
        src/Assert.cpp
        src/Asset/AssetManager.cpp
        src/Asset/DynamicMesh.cpp
        src/Asset/Level.cpp
        src/Asset/Material.cpp
        src/Asset/MaterialInstance.cpp
        src/Asset/Mesh.cpp
        src/Asset/Environment.cpp
        src/Asset/Font.cpp
        src/Asset/InputMappingContext.cpp
        src/Asset/Prefab.cpp
        src/Asset/Primitives.cpp
        src/Asset/ResidencyBatch.cpp
        src/Asset/Skeleton.cpp
        src/Asset/Texture.cpp
        src/Asset/Loaders/AnimationLoader.cpp
        src/Asset/Loaders/EnvironmentLoader.cpp
        src/Asset/Loaders/FontLoader.cpp
        src/Asset/Loaders/InputMapLoader.cpp
        src/Asset/Loaders/StyleSheetLoader.cpp
        src/Asset/Loaders/UIDocumentLoader.cpp
        src/Asset/Loaders/LevelLoader.cpp
        src/Asset/Loaders/MaterialLoader.cpp
        src/Asset/Loaders/MaterialInstanceLoader.cpp
        src/Asset/Loaders/MeshLoader.cpp
        src/Asset/Loaders/PrefabLoader.cpp
        src/Asset/Loaders/RawAssetLoader.cpp
        src/Asset/Loaders/ShaderLoader.cpp
        src/Asset/Loaders/SkeletonLoader.cpp
        src/Asset/Loaders/TextureLoader.cpp
        src/Asset/Loaders/VertexLayoutLoader.cpp
        src/ExecutableDirectory.cpp
        src/Input.cpp
        src/Input/Actions.cpp
        src/Input/RawInput.cpp
        src/Input/SeatFocusScope.cpp
        src/InputRouter.cpp
        src/LevelOverlay.cpp
        src/LaunchArguments.cpp
        src/Log.cpp
        src/ManagedViewports.cpp
        src/Math/AABB.cpp
        src/Math/BVH.cpp
        src/Math/Frustum.cpp
        src/Math/Noise.cpp
        src/Module/ApplicationRegistry.cpp
        src/Module/ModuleLoader.cpp
        src/Net/AccountId.cpp
        src/Net/Client.cpp
        src/Net/ClockSync.cpp
        src/Net/Connection.cpp
        src/Net/DeltaCodec.cpp
        src/Net/FaultInjectionTransport.cpp
        src/Net/Quantize.cpp
        src/Net/Handshake.cpp
        src/Net/Host.cpp
        src/Net/Interest.cpp
        src/Net/PredictionHistory.cpp
        src/Net/Reconciliation.cpp
        src/Net/InputFeed.cpp
        src/Net/LoopbackTransport.cpp
        src/Net/Server.cpp
        src/Net/UdpTransport.cpp
        src/Platform/UserPaths.cpp
        src/Task/TaskSystem.cpp
        src/Time.cpp
        src/Window.cpp
        src/WorldDirectory.cpp
        src/WorldRunner.cpp
        src/ImGui/ImGuiLayer.cpp
        src/ImGui/ImGuiTexture.cpp
        src/UI/Theme.cpp
        src/UI/Widgets.cpp
        src/UI/Layout.cpp
        src/UI/Scopes.cpp
        src/UI/Joined.cpp
        src/UI/Query.cpp
        src/UI/DebugPanels.cpp
        src/UI/Inspector.cpp
        src/UI/RenderSettings.cpp
        src/Net/InputReplication.cpp
        src/Net/Replication.cpp
        src/Reflection/FieldDisplay.cpp
        src/Reflection/FieldGate.cpp
        src/Reflection/Serialize.cpp
        src/Reflection/JsonSerialize.cpp
        src/Scene/AnimationSystem.cpp
        src/Scene/BuiltinSystems.cpp
        src/Scene/BuiltinTypes.cpp
        src/Scene/Camera.cpp
        src/Scene/CameraRig.cpp
        src/Scene/ComponentPool.cpp
        src/Scene/DeviceAssignmentSystem.cpp
        src/Scene/FocusRequestReconcile.cpp
        src/Scene/InputMappingSystem.cpp
        src/Scene/Motion.cpp
        src/Scene/Movement.cpp
        src/Scene/RemoteInterpolationSystem.cpp
        src/Scene/RequestDrain.cpp
        src/Scene/Resolve.cpp
        src/Scene/RootMotion.cpp
        src/Scene/Scene.cpp
        src/Scene/SceneBroadphase.cpp
        src/Scene/SceneSimulation.cpp
        src/Scene/SceneSystem.cpp
        src/Scene/SceneViewport.cpp
        src/Scene/SystemRegistry.cpp
        src/Scene/TimeOfDay.cpp
        src/Scene/Transforms.cpp
        src/Scene/Visibility.cpp
        src/Renderer/SwapChainCompositePass.cpp
        src/Renderer/GatherPass.cpp
        src/Gui/BindingContext.cpp
        src/Gui/DrawList.cpp
        src/Gui/Document.cpp
        src/Gui/DocumentHost.cpp
        src/Gui/DocumentLayer.cpp
        src/Gui/DocumentTexture.cpp
        src/Gui/DriverRegistry.cpp
        src/Gui/GuiConsumer.cpp
        src/Gui/Overlay.cpp
        src/Gui/Placement.cpp
        src/Gui/RenderTarget.cpp
        src/Gui/Surface.cpp
        src/Gui/SurfaceInput.cpp
        src/Gui/StyleProperty.cpp
        src/Gui/StyleSheet.cpp
        src/Gui/UIDocument.cpp
        src/Gui/YogaTree.cpp
        src/Renderer/Passes/GuiScenePass.cpp
        src/Renderer/DebugDraw.cpp
        src/Renderer/DebugDrawScenePass.cpp
        src/Renderer/PointField.cpp
        src/Renderer/Passes/PointFieldScenePass.cpp
        src/Renderer/VolumeField.cpp
        src/Renderer/VolumeMarch.cpp
        src/Renderer/Passes/VolumeScenePass.cpp
        src/Renderer/EnvironmentIbl.cpp
        src/Renderer/RenderGraph.cpp
        src/Renderer/SceneCapture.cpp
        src/Renderer/CaptureSurface.cpp
        src/Renderer/SceneRenderer.cpp
        src/Renderer/Viewport.cpp
        src/Renderer/ViewportCompositor.cpp
        src/Renderer/ViewportRegistry.cpp
        src/Renderer/LightPacking.cpp
        src/Renderer/LtcLut.cpp
        src/Renderer/ShadowCascades.cpp
        src/Renderer/HiZHistory.cpp
        src/Renderer/PunctualShadows.cpp
        src/Renderer/ShadowScenePass.cpp
        src/Renderer/PunctualShadowScenePass.cpp
        src/Renderer/SkyboxScenePass.cpp
        src/Renderer/SkyCubemapBake.cpp
        src/Renderer/Passes/AtmospherePrecompute.cpp
        src/Renderer/Passes/SkyScenePass.cpp
        src/Renderer/SsaoScenePass.cpp
        src/Renderer/ShaderInterface.cpp
        $<TARGET_OBJECTS:veng_backend_objs>
        src/Vendor/ImGui.cpp
        src/Vendor/TinyExr.cpp
)
add_library(veng::veng ALIAS veng)

target_include_directories(veng PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# ImGui/imnodes headers are part of veng's public API surface
# (Veng/Vendor/ImGui.h includes <imgui.h>, <imnodes.h>, ...).
target_include_directories(veng PUBLIC
        $<BUILD_INTERFACE:${imgui_SOURCE_DIR}>
        $<BUILD_INTERFACE:${imnodes_SOURCE_DIR}>
)

# Inject glm<->ImVec2/ImVec4 conversions into ImGui's own vector types. PUBLIC so
# imgui's aggregation TU and every consumer compile one identical ImVec2/ImVec4.
target_compile_definitions(veng PUBLIC
        IMGUI_USER_CONFIG="Veng/Vendor/ImGuiConfig.h")

# tinyexr/stb headers are implementation details of the vendor TUs.
target_include_directories(veng PRIVATE
        ${tinyexr_SOURCE_DIR}
        ${stb_SOURCE_DIR}
)

target_compile_definitions(veng PRIVATE VE_BUILD_SHARED)

# The VE_API dllexport annotations cover only part of the public surface (the Windows
# port is in progress), while macOS/Linux export everything via default visibility. On
# MSVC, auto-export every symbol so the shared library matches that behaviour and its
# consumers (game modules, vengc, the editor) link without the annotation being complete.
if (MSVC)
    set_target_properties(veng PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif ()

# vulkan-hpp's dynamic dispatcher (VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1) keeps its
# storage (defaultDispatchLoaderDynamic) in one TU, here in veng.dll (Context.cpp). It
# is a data symbol, so sharing it across the DLL boundary on Windows needs explicit
# dllexport/dllimport: STORAGE_SHARED everywhere, STORAGE_SHARED_EXPORT only where the
# storage lives. Default visibility already handles this on macOS/Linux.
if (WIN32)
    target_compile_definitions(veng PUBLIC VULKAN_HPP_STORAGE_SHARED)
    target_compile_definitions(veng PRIVATE VULKAN_HPP_STORAGE_SHARED_EXPORT)
endif ()

# veng raises no exceptions (see Veng/Assert.h): asserts are fatal, recoverable
# operations return Veng::Result<T>. Building -fno-exceptions is the enforcement
# mechanism — any stray `throw` becomes a compile error. tinyexr is the one
# vendor TU that uses exceptions by default, so disable them for it too.
target_compile_options(veng PRIVATE -fno-exceptions)
set_source_files_properties(src/Vendor/TinyExr.cpp PROPERTIES
        COMPILE_DEFINITIONS TINYEXR_USE_EXCEPTIONS=0)

if (VE_DEBUG)
    target_compile_definitions(veng PUBLIC VE_ENABLE_VALIDATION_LAYERS=1 VE_DEBUG=1)
endif ()

# glm and fmt appear in veng's public headers (engine vector/matrix types and
# string formatting), so they stay PUBLIC. veng::assetpack is PUBLIC too —
# AssetId and friends appear in the public AssetManager API. nlohmann/json is
# PUBLIC too — the reflection JSON walker (Veng/Reflection/JsonSerialize.h) names
# json types directly, so every consumer of that header needs the include resolved
# through this link. Vulkan, GLFW, VMA and nfd are hidden behind the Native idiom
# (see Veng/Renderer/Native.h) — no public header pulls them in, so they link
# PRIVATE and stay out of the consumer include graph.
target_link_libraries(veng PUBLIC
        glm::glm
        fmt::fmt
        veng::assetpack
        nlohmann_json::nlohmann_json
)

target_link_libraries(veng PRIVATE
        Vulkan::Vulkan
        glfw
        GPUOpen::VulkanMemoryAllocator
        nfd
        ZLIB::ZLIB
        yogacore
        ${CMAKE_DL_LIBS}
)

# Veng.h is the foundational header every TU includes (std + glm + fmt + house
# aliases); precompiling it once cuts that front-end cost across the library.
target_precompile_headers(veng PRIVATE include/Veng/Veng.h)

# The vendor aggregation TUs #include third-party .c/.cpp and set feature macros
# before their includes; force-injecting Veng.h ahead of that is unsafe, and the
# generated embed TUs are bare byte arrays that gain nothing from it.
set_source_files_properties(
        src/Vendor/ImGui.cpp
        src/Vendor/TinyExr.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
