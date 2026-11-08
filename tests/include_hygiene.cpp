// Include-hygiene guard for the public/backend header split.
//
// This TU includes every public Veng header and is compiled into a target that
// links veng::veng but is NOT given the Vulkan / GLFW / VMA / nfd include
// directories (those link PRIVATE to veng). If any public header regresses and
// pulls in <vulkan/...>, <GLFW/...>, vk_mem_alloc.h or <nfd.h>, this file fails
// to compile — keeping the boundary from rotting.
//
// Veng/Renderer/Native.h is deliberately excluded: it is the one sanctioned
// escape hatch that exposes raw handles and is expected to need Vulkan headers.
//
// Compiling catches an include that fails to resolve; it cannot catch a header naming a backend
// type it never needs complete, or a backend header reached through a relative path. So the
// binary also scans every public engine header's text, and fails on a backend include or a
// Vulkan / VMA name (vk::, Vk*, VK_*, Vma*, vma*) anywhere in it — comments included, since a
// public doc comment states the contract in engine terms. Native.h is excluded there too.
//
// The same rule holds inside the engine: only engine/src/Renderer/Backend/ includes a backend
// header or Native.h, so the scan also walks every other engine source and fails on such an
// include there.
//
// The Veng/Asset/* headers come from libveng_assetpack,
// linked PUBLIC by veng — this sweep also proves that boundary stays
// Vulkan-free.

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Asset/NormalVariance.h>

#include <Veng/Asset/AssetError.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CollisionShape.h>
#include <Veng/Asset/DataTable.h>
#include <Veng/Asset/Font.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Asset/Level.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Asset/RawAsset.h>
#include <Veng/Asset/ResidencyBatch.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Asset/VertexLayout.h>

#include <Veng/Application.h>
#include <Veng/Assert.h>

#include <Veng/Audio/AudioBuffer.h>
#include <Veng/Audio/AudioBus.h>
#include <Veng/Audio/AudioClip.h>
#include <Veng/Audio/AudioComponents.h>
#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Audio/AudioGenerator.h>
#include <Veng/Audio/AudioSystem.h>
#include <Veng/Audio/Dsp.h>
#include <Veng/Audio/Reverb.h>
#include <Veng/Audio/ScopedAudio.h>
#include <Veng/Audio/Voice.h>

#include <Veng/Behavior/BehaviorAgent.h>
#include <Veng/Behavior/BehaviorSystem.h>
#include <Veng/Behavior/BehaviorTree.h>

#include <Veng/Capture/VideoRecorder.h>

#include <Veng/Diagnostics/FileTraceSink.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Diagnostics/TraceSink.h>

#include <Veng/Event.h>
#include <Veng/FrameClock.h>
#include <Veng/Haptics/Haptics.h>
#include <Veng/Haptics/HapticsSystem.h>
#include <Veng/Haptics/RumbleClip.h>
#include <Veng/Haptics/RumbleSource.h>
#include <Veng/Haptics/ScopedHaptics.h>
#include <Veng/Input.h>
#include <Veng/Input/Actions.h>
#include <Veng/Input/RawInput.h>
#include <Veng/LaunchArguments.h>
#include <Veng/Log.h>
#include <Veng/Result.h>
#include <Veng/Task/ParallelFor.h>
#include <Veng/Text/CaseMapping.h>
#include <Veng/Text/GlyphAtlas.h>
#include <Veng/Text/GlyphSource.h>
#include <Veng/Time.h>
#include <Veng/Veng.h>
#include <Veng/Window.h>

#include <Veng/Render/DisplayCapabilities.h>
#include <Veng/Render/DisplayModes.h>
#include <Veng/Render/FrameRateLimiter.h>
#include <Veng/WindowEvents.h>
#include <Veng/WorldDirectory.h>

#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/ImGui/ImGuiTexture.h>

#include <Veng/Math/AABB.h>
#include <Veng/Math/BVH.h>
#include <Veng/Math/CubeSphere.h>
#include <Veng/Math/Curve.h>
#include <Veng/Math/Ease.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Math/Noise.h>
#include <Veng/Math/Random.h>
#include <Veng/Math/SphericalHarmonics.h>
#include <Veng/Math/Steering.h>

#include <Veng/UI/UI.h>
#include <Veng/UI/Types.h>
#include <Veng/UI/Theme.h>
#include <Veng/UI/Widgets.h>
#include <Veng/UI/Layout.h>
#include <Veng/UI/Scopes.h>
#include <Veng/UI/Query.h>
#include <Veng/UI/DebugPanels.h>
#include <Veng/UI/GamepadPanel.h>
#include <Veng/UI/VideoCapture.h>

#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/Style.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/Placement.h>
#include <Veng/Gui/RenderTarget.h>
#include <Veng/Gui/Surface.h>
#include <Veng/Gui/SurfaceInput.h>

#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Module/Module.h>
#include <Veng/Module/ModuleLoader.h>

#include <Veng/Net/AccountId.h>
#include <Veng/Net/Blob.h>
#include <Veng/Net/BlobCodec.h>
#include <Veng/Net/ClockSync.h>
#include <Veng/Net/Connection.h>
#include <Veng/Net/FaultInjectionTransport.h>
#include <Veng/Net/JoinRequest.h>
#include <Veng/Net/LagCompensation.h>
#include <Veng/Net/LoopbackTransport.h>
#include <Veng/Net/Messages.h>
#include <Veng/Net/PredictionHistory.h>
#include <Veng/Net/SeatRelease.h>
#include <Veng/Net/Session.h>
#include <Veng/Net/Social.h>
#include <Veng/Net/Transport.h>
#include <Veng/Net/UdpTransport.h>
#include <Veng/Net/WorldKey.h>

#include <Veng/Persistence/LocalAccountStore.h>
#include <Veng/Persistence/SaveSlots.h>
#include <Veng/Persistence/SessionStore.h>
#include <Veng/Persistence/Store.h>

#include <Veng/Physics/Components.h>
#include <Veng/Physics/Gravity.h>
#include <Veng/Physics/Layers.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Physics/PoseResolver.h>
#include <Veng/Physics/Queries.h>

#include <Veng/Platform/UserPaths.h>

#include <Veng/Reflection/EnumName.h>
#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/ReflectionTypes.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Reflection/Variant.h>

#include <Veng/Project/BuildConfiguration.h>
#include <Veng/Project/CompressionFormat.h>
#include <Veng/Project/CompressionRole.h>
#include <Veng/Project/ProjectSettings.h>

#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/ComponentPool.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Components/SphereTerrain.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/InputMappingSystem.h>
#include <Veng/Scene/PresentationScope.h>
#include <Veng/Scene/Requests.h>
#include <Veng/Scene/Resolve.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Sockets.h>
#include <Veng/Scene/Transforms.h>

#include <Veng/Renderer/AsyncReadback.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CaptureSink.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/DeviceProbe.h>
#include <Veng/Renderer/Fence.h>
#include <Veng/Renderer/FluidSim.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GeneratedTextureService.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/SceneRendererSettings.h>
#include <Veng/Renderer/SceneView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/ShadowCascades.h>
#include <Veng/Renderer/Semaphore.h>
#include <Veng/Renderer/ShaderModule.h>
#include <Veng/Renderer/SphereTerrain.h>
#include <Veng/Renderer/SwapChainCompositePass.h>
#include <Veng/Renderer/TypedBuffers.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/VertexBufferLayout.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportDevices.h>
#include <Veng/Renderer/ViewportId.h>
#include <Veng/Renderer/ViewportRegistry.h>
#include <Veng/Renderer/VolumeField.h>

#include <VengGraph/NodeGraph.h>
#include <VengGraph/NodeType.h>
#include <VengGraph/NodeGraphSerialize.h>
#include <VengGraph/EmittedValue.h>
#include <VengGraph/MaterialShaderInterface.h>
#include <VengGraph/MaterialCatalog.h>
#include <VengGraph/MaterialCompile.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

int main()
{
    namespace fs = std::filesystem;
    const fs::path root = VENG_PUBLIC_INCLUDE_DIR;
    const fs::path exempt = root / "Veng" / "Renderer" / "Native.h";
    const std::regex backendName{
        R"(\bvk::|\bVk[A-Z]\w*|\bVK_[A-Z]\w*|\bVma[A-Z]\w*|\bvma[A-Z]\w*|vulkan/|vk_mem_alloc|Renderer/Backend/)"};

    int failures = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".h" ||
            fs::equivalent(entry.path(), exempt))
        {
            continue;
        }
        std::ifstream file(entry.path());
        std::string line;
        int number = 0;
        while (std::getline(file, line))
        {
            ++number;
            if (std::regex_search(line, backendName))
            {
                std::fprintf(stderr, "%s:%d names the GPU backend: %s\n",
                             entry.path().string().c_str(), number, line.c_str());
                ++failures;
            }
        }
    }

    const fs::path engineSource = VENG_ENGINE_SOURCE_DIR;
    const fs::path backendSource = engineSource / "Renderer" / "Backend";
    const std::regex backendInclude{
        R"(^\s*#\s*(include|import)\s*[<"].*(Backend/|vulkan/|vk_mem_alloc|Veng/Renderer/Native\.h))"};
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(engineSource))
    {
        const fs::path extension = entry.path().extension();
        if (!entry.is_regular_file() ||
            (extension != ".h" && extension != ".cpp" && extension != ".mm"))
        {
            continue;
        }
        const fs::path relative = entry.path().lexically_relative(backendSource);
        if (!relative.empty() && *relative.begin() != "..")
        {
            continue;
        }
        std::ifstream file(entry.path());
        std::string line;
        int number = 0;
        while (std::getline(file, line))
        {
            ++number;
            if (std::regex_search(line, backendInclude))
            {
                std::fprintf(stderr, "%s:%d includes the GPU backend outside it: %s\n",
                             entry.path().string().c_str(), number, line.c_str());
                ++failures;
            }
        }
    }
    return failures == 0 ? 0 : 1;
}
