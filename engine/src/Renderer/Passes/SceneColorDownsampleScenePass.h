#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Veng.h>

#include <string>

#include "SceneColorCopyScenePass.h"

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

    /// @brief Construction parameters for one level's SceneColorDownsampleScenePass.
    struct SceneColorDownsamplePassInfo
    {
        /// @brief The graph pass name (what DidRecordPassLastFrame reports).
        string Name;
        /// @brief The single-attachment downsample pipeline.
        Ref<GraphicsPipeline> Pipeline;
        /// @brief The parent level's resource id (declared sampled for barrier order).
        ResourceId SourceId;
        /// @brief Bindless slot for the parent level.
        TextureHandle SourceHandle;
        /// @brief The level this pass writes.
        ResourceId TargetId;
        /// @brief The chain sampler's bindless slot.
        SamplerHandle Sampler;
        /// @brief This pass's destination level, 1 or deeper.
        u32 Level = 1;
        /// @brief The chain's base allocation extent; updated via Resize.
        uvec2 Extent{0, 0};
        /// @brief The frame's flag, set when something samples the chain this frame; the pass skips
        ///        every other frame, with the chain.
        const bool* Sampled = nullptr;
        /// @brief Which part of the chain's base holds content each frame.
        SceneColorChainRegion Region = SceneColorChainRegion::RenderSubRect;
    };

    /// @brief Halves one level of a scene-colour chain into the next.
    ///
    /// One of these per level below the base, run in order straight after the copy pass, so a
    /// consumer can sample the scene behind it blurred by reading a coarser level. Each renders only
    /// the sub-rect its parent occupied, halved, and clamps its reads inside the parent's valid
    /// region — the same dynamic-resolution discipline the copy takes, applied at every level so the
    /// cleared area outside the sub-rect never works its way inward.
    class SceneColorDownsampleScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs one level's pass.
        /// @param context Renderer context for bindless access.
        /// @param info    The level's source, target, pipeline and per-frame flag.
        SceneColorDownsampleScenePass(Context& context, SceneColorDownsamplePassInfo info)
            : m_Context(context), m_Pipeline(std::move(info.Pipeline)), m_SourceId(info.SourceId),
              m_SourceHandle(info.SourceHandle), m_TargetId(info.TargetId), m_Sampler(info.Sampler),
              m_Level(info.Level), m_Extent(info.Extent), m_Sampled(info.Sampled),
              m_Region(info.Region), m_Name(std::move(info.Name))
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
        /// @brief The frame's grab-sampled flag (borrowed from the renderer).
        const bool* m_Sampled;
        /// @brief Which part of the chain's base holds content each frame.
        SceneColorChainRegion m_Region;
        /// @brief This pass's graph name, owned here because AddPass takes a view.
        string m_Name;
    };
}
