#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Result.h>

namespace Veng
{
    class AssetManager;
    struct Shader;
}

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;
    class PipelineLayout;
}

namespace Veng::Detail
{
    /// @brief The core pack's skinned surface vertex stage (surface_skinned.vert).
    ///
    /// The canonical surface vertex stage plus 4-influence linear-blend skinning, reading the
    /// per-instance palette at set 2. Every skinned g-buffer pipeline is built on it.
    inline constexpr AssetId SurfaceSkinnedVertId{0x984BE76D55A4DA7CULL};

    /// @brief Builds a g-buffer pipeline from a vertex and a fragment stage over a given layout.
    ///
    /// Targets the deferred g-buffer's five MRT formats and depth with depth test and write on, and
    /// reads the per-draw candidate id as an instance-rate attribute. The vertex buffer layout is
    /// the one the vertex stage's reflection names. Every Surface g-buffer pipeline is built here,
    /// static or skinned, so they cannot drift apart in anything but their stages and layout.
    /// @param manager   Asset manager, used to load the vertex stage's VertexLayout.
    /// @param context   Render context the pipeline is created on.
    /// @param name      The pipeline's debug name.
    /// @param layout    The pipeline layout the stages are bound against.
    /// @param vsAsset   The (resident) vertex shader.
    /// @param fsAsset   The (resident) fragment shader.
    /// @param cullMode  The face-culling mode.
    /// @return The built pipeline, or a recoverable error string.
    Result<Ref<Renderer::GraphicsPipeline>>
    BuildSurfacePipeline(AssetManager& manager, Renderer::Context& context, string name,
                         const Ref<Renderer::PipelineLayout>& layout, const Veng::Shader& vsAsset,
                         const Veng::Shader& fsAsset, Renderer::CullMode cullMode);

    /// @brief Builds a Surface material's skinned g-buffer pipeline from its fragment shader.
    ///
    /// Pairs the core skinned surface vertex stage (surface_skinned.vert — 4-influence linear-blend
    /// skinning, the per-instance palette at set 2) with @p fragmentShader against the g-buffer's
    /// five MRT formats, reflecting a three-set layout (set 0 bindless, set 1 DrawData, set 2 the
    /// palette). The layout's palette set is what makes the geometry pass's set-2 bind valid for a
    /// skinned draw. Called lazily by Material::EnsureSkinnedPipeline on the render thread, so a
    /// material never drawn skinned never builds it. Shares the material loader's own surface-build
    /// path, so a fragment consuming the full surface interpolant set links exactly as the static
    /// pipeline does.
    /// @param manager        Asset manager, used to load the skinned vertex shader and its layout.
    /// @param context        Render context the pipeline is created on.
    /// @param id             The material's AssetId, used only for the debug names.
    /// @param fragmentShader The material's (resident) fragment shader.
    /// @param cullMode       The material's authored face-culling mode.
    /// @return The built skinned pipeline, or a recoverable error string.
    Result<Ref<Renderer::GraphicsPipeline>>
    BuildSkinnedSurfacePipeline(AssetManager& manager, Renderer::Context& context, AssetId id,
                                const Veng::Shader& fragmentShader, Renderer::CullMode cullMode);
}
