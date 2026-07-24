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
}

namespace Veng::Detail
{
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
