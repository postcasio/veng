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
// The Veng/Asset/* headers come from libveng_assetpack,
// linked PUBLIC by veng — this sweep also proves that boundary stays
// Vulkan-free.

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/CookedBlobs.h>

#include <Veng/Asset/AssetError.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Level.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Asset/RawAsset.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Asset/VertexLayout.h>

#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Event.h>
#include <Veng/Input.h>
#include <Veng/Log.h>
#include <Veng/Result.h>
#include <Veng/Time.h>
#include <Veng/Veng.h>
#include <Veng/Window.h>
#include <Veng/WindowEvents.h>

#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/ImGui/ImGuiTexture.h>

#include <Veng/Math/AABB.h>
#include <Veng/Math/BVH.h>
#include <Veng/Math/Frustum.h>

#include <Veng/UI/UI.h>
#include <Veng/UI/Types.h>
#include <Veng/UI/Theme.h>
#include <Veng/UI/Widgets.h>
#include <Veng/UI/Layout.h>
#include <Veng/UI/Scopes.h>
#include <Veng/UI/Query.h>

#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Module/Module.h>
#include <Veng/Module/ModuleLoader.h>

#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/ReflectionTypes.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Reflection/Variant.h>

#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Resolve.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Fence.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/ShadowCascades.h>
#include <Veng/Renderer/Semaphore.h>
#include <Veng/Renderer/ShaderModule.h>
#include <Veng/Renderer/SwapChainCompositePass.h>
#include <Veng/Renderer/TypedBuffers.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/VertexBufferLayout.h>

#include <VengEditor/NodeGraph/NodeGraph.h>
#include <VengEditor/NodeGraph/NodeType.h>
#include <VengEditor/NodeGraph/NodeGraphSerialize.h>

int main()
{
    return 0;
}
