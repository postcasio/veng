#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Result.h>

namespace Veng
{
    class AssetManager;
    struct Shader;
    enum class MaterialDomain : u32;
}

namespace Veng::Renderer
{
    class Context;
    class DescriptorSetLayout;
    class GraphicsPipeline;
    class PipelineLayout;
    struct ShaderInterface;
}

namespace Veng::Detail
{
    /// @brief The core pack's skinned surface vertex stage (surface_skinned.vert).
    ///
    /// The canonical surface vertex stage plus 4-influence linear-blend skinning, reading the
    /// per-instance palette at set 4. Every skinned g-buffer pipeline is built on it.
    inline constexpr AssetId SurfaceSkinnedVertId{0x984BE76D55A4DA7CULL};

    /// @brief The core pack's instanced-set surface vertex stage (surface_instanced.vert).
    ///
    /// The canonical surface vertex stage placing each vertex by its instance's transform composed
    /// into the set's frame. Every instanced-set g-buffer pipeline is built on it.
    inline constexpr AssetId SurfaceInstancedVertId{0x2D1ACA1F99BD2790ULL};

    /// @brief Whether a material's fragment reads the shadow maps: a Surface fragment declaring the
    ///        shadow set (SurfaceShadowMapSet).
    /// @param domain   The material's domain.
    /// @param fragment The fragment stage's reflected interface.
    [[nodiscard]] bool FragmentReadsShadowMaps(MaterialDomain domain,
                                               const Renderer::ShaderInterface& fragment);

    /// @brief Builds the layout a Surface material's g-buffer pipeline over a vertex stage binds
    ///        against.
    ///
    /// The stages' reflected sets from 3 on, set 4 the terrain's own for a sphere-terrain material.
    /// For a fragment reading the shadow maps the reflected shadow set is dropped, set 4 is an empty
    /// layout where the stages declare none, and set 5 is @p shadowLayout when it is given and
    /// absent otherwise: the material's own layout, which no pipeline reading the maps is built
    /// against, carries sets 3 and 4 alone.
    /// @param context       Render context the layouts are created on.
    /// @param id            The material's AssetId, used only for the debug names.
    /// @param vsAsset       The (resident) vertex shader.
    /// @param fsAsset       The (resident) fragment shader.
    /// @param sphereTerrain Whether the material shades sphere terrain.
    /// @param shadowLayout  The renderer's shadow set layout, or null.
    /// @return The layout, or a recoverable error string.
    [[nodiscard]] Result<Ref<Renderer::PipelineLayout>>
    BuildSurfacePipelineLayout(Renderer::Context& context, AssetId id, const Veng::Shader& vsAsset,
                               const Veng::Shader& fsAsset, bool sphereTerrain,
                               const Ref<Renderer::DescriptorSetLayout>& shadowLayout = nullptr);

    /// @brief Builds a g-buffer pipeline from a vertex and a fragment stage over a given layout.
    ///
    /// Targets the deferred g-buffer's five MRT formats and depth with depth test and write on, and
    /// reads the per-draw candidate id as an instance-rate attribute unless told otherwise. The
    /// vertex buffer layout is the one the vertex stage's reflection names. Every Surface g-buffer
    /// pipeline is built here, static, skinned or instanced-set, so they cannot drift apart in
    /// anything but their stages, layout and instance binding.
    /// @param manager   Asset manager, used to load the vertex stage's VertexLayout.
    /// @param context   Render context the pipeline is created on.
    /// @param name      The pipeline's debug name.
    /// @param layout    The pipeline layout the stages are bound against.
    /// @param vsAsset   The (resident) vertex shader.
    /// @param fsAsset   The (resident) fragment shader.
    /// @param cullMode  The face-culling mode.
    /// @param instanceCandidateId Whether binding 1 carries the instance-rate candidate id; false
    ///                  for the instanced-set stage, which reads no instance-rate attribute.
    /// @return The built pipeline, or a recoverable error string.
    Result<Ref<Renderer::GraphicsPipeline>>
    BuildSurfacePipeline(AssetManager& manager, Renderer::Context& context, string name,
                         const Ref<Renderer::PipelineLayout>& layout, const Veng::Shader& vsAsset,
                         const Veng::Shader& fsAsset, Renderer::CullMode cullMode,
                         bool instanceCandidateId = true);

    /// @brief Builds a Surface material's skinned g-buffer pipeline from its fragment shader.
    ///
    /// Pairs the core skinned surface vertex stage (surface_skinned.vert — 4-influence linear-blend
    /// skinning, the per-instance palette at set 4) with @p fragmentShader against the g-buffer's
    /// five MRT formats, reflecting a layout with the palette set (sets 0–2 bindless, set 3 DrawData,
    /// set 4 the palette). That set is what makes the geometry pass's set-4 bind valid for a
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

    /// @brief Builds a Surface material's instanced-set g-buffer pipeline from its fragment shader.
    ///
    /// Pairs the core instanced-set vertex stage (surface_instanced.vert) with @p fragmentShader
    /// against the g-buffer's five MRT formats, with no instance-rate binding. Called lazily by
    /// Material::EnsureInstancedPipeline on the render thread for a material declaring
    /// "instancedSets", so a material no set draws never builds it.
    /// @param manager        Asset manager, used to load the instanced vertex shader and its layout.
    /// @param context        Render context the pipeline is created on.
    /// @param id             The material's AssetId, used only for the debug names.
    /// @param fragmentShader The material's (resident) fragment shader.
    /// @param cullMode       The material's authored face-culling mode.
    /// @return The built instanced-set pipeline, or a recoverable error string.
    Result<Ref<Renderer::GraphicsPipeline>>
    BuildInstancedSurfacePipeline(AssetManager& manager, Renderer::Context& context, AssetId id,
                                  const Veng::Shader& fragmentShader, Renderer::CullMode cullMode);
}
