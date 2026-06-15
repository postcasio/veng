# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(veng SHARED
        src/Application.cpp
        src/Assert.cpp
        src/Asset/AssetManager.cpp
        src/Asset/Material.cpp
        src/Asset/Mesh.cpp
        src/Asset/Primitives.cpp
        src/Asset/Texture.cpp
        src/Asset/Loaders/MaterialLoader.cpp
        src/Asset/Loaders/MeshLoader.cpp
        src/Asset/Loaders/RawAssetLoader.cpp
        src/Asset/Loaders/ShaderLoader.cpp
        src/Asset/Loaders/TextureLoader.cpp
        src/Asset/Loaders/VertexLayoutLoader.cpp
        src/ExecutableDirectory.cpp
        src/Log.cpp
        src/Module/ApplicationRegistry.cpp
        src/Module/ModuleLoader.cpp
        src/Task/TaskSystem.cpp
        src/Time.cpp
        src/Window.cpp
        src/ImGui/ImGuiLayer.cpp
        src/ImGui/ImGuiTexture.cpp
        src/Reflection/Serialize.cpp
        src/Scene/BuiltinTypes.cpp
        src/Scene/ComponentPool.cpp
        src/Scene/Scene.cpp
        src/Scene/Transforms.cpp
        src/Renderer/RenderGraph.cpp
        src/Renderer/ShaderInterface.cpp
        src/Renderer/Backend/Barrier.cpp
        src/Renderer/Backend/BarrierDecision.cpp
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
        src/Vendor/ImGui.cpp
        src/Vendor/StbImage.cpp
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

# tinyexr/stb headers are implementation details of the vendor TUs.
target_include_directories(veng PRIVATE
        ${tinyexr_SOURCE_DIR}
        ${stb_SOURCE_DIR}
)

target_compile_definitions(veng PRIVATE VE_BUILD_SHARED)

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
# string formatting), so they stay PUBLIC. veng::assetformat is PUBLIC too —
# AssetId and friends appear in the public AssetManager API. Vulkan,
# GLFW, VMA and nfd are hidden behind the Native idiom (see
# Veng/Renderer/Native.h) — no public header pulls them in, so they link
# PRIVATE and stay out of the consumer include graph.
target_link_libraries(veng PUBLIC
        glm::glm
        fmt::fmt
        veng::assetformat
)

target_link_libraries(veng PRIVATE
        Vulkan::Vulkan
        glfw
        GPUOpen::VulkanMemoryAllocator
        nfd
        ZLIB::ZLIB
        ${CMAKE_DL_LIBS}
)
