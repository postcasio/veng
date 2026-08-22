#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Veng.h>

#include <string>

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;

    // The downsample push block, matching scene_color_downsample.frag PushConstants: the parent
    // level's bindless slot, the chain sampler, and the parent's dynamic-resolution sub-rect
    // mapping plus its texel size (the offset the four taps are spread by).
    struct SceneColorDownsamplePush
    {
        u32 SourceTexture;
        u32 Sampler;
        u32 Pad0;
        u32 Pad1;
        vec2 ScaleUV;
        vec2 MaxUV;
        vec2 TexelSize;
    };

    /// @brief Halves one level of the refraction scene-color grab into the next.
    ///
    /// One of these per level below the base, run in order straight after the copy pass, so a
    /// translucent material can sample the scene behind it blurred by reading a coarser level. Each
    /// renders only the sub-rect its parent occupied, halved, and clamps its reads inside the
    /// parent's valid region — the same dynamic-resolution discipline the copy takes, applied at
    /// every level so the cleared area outside the sub-rect never works its way inward.
    class SceneColorDownsampleScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs one level's pass.
        /// @param context       Renderer context for bindless access.
        /// @param pipeline      The single-attachment downsample pipeline.
        /// @param sourceId      The parent level's resource id (declared sampled for barrier order).
        /// @param sourceHandle  Bindless slot for the parent level.
        /// @param targetId      The level this pass writes.
        /// @param sampler       The chain sampler's bindless slot.
        /// @param level         This pass's destination level, 1 or deeper.
        /// @param extent        The grab's base allocation extent; updated via Resize.
        SceneColorDownsampleScenePass(Context& context, Ref<GraphicsPipeline> pipeline,
                                      ResourceId sourceId, TextureHandle sourceHandle,
                                      ResourceId targetId, SamplerHandle sampler, u32 level,
                                      uvec2 extent)
            : m_Context(context), m_Pipeline(std::move(pipeline)), m_SourceId(sourceId),
              m_SourceHandle(sourceHandle), m_TargetId(targetId), m_Sampler(sampler),
              m_Level(level), m_Extent(extent),
              m_Name("Scene Color Downsample " + std::to_string(level))
        {
        }

        /// @brief Updates the grab's base allocation extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes this level's halving pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief One level's extent, floored at a single texel so a deep chain cannot reach zero.
        [[nodiscard]] static uvec2 LevelExtent(uvec2 base, u32 level);

        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The single-attachment downsample pipeline.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief The parent level's resource id.
        ResourceId m_SourceId;
        /// @brief Bindless slot for the parent level.
        TextureHandle m_SourceHandle;
        /// @brief The level this pass writes.
        ResourceId m_TargetId;
        /// @brief The chain sampler's bindless slot.
        SamplerHandle m_Sampler;
        /// @brief This pass's destination level, 1 or deeper.
        u32 m_Level;
        /// @brief The grab's base allocation extent.
        uvec2 m_Extent;
        /// @brief This pass's graph name, owned here because AddPass takes a view.
        string m_Name;
    };
}
