#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;
    class DescriptorSet;
    class GraphicsPipeline;

    // The deferred-lighting fragment push block: the g-buffer bindless slots (including the G4
    // emissive read), the shared sampler, and the view-constants index. The light state — light and
    // area-vertex bases, count, ambient arm and parameters, LTC LUTs — rides the view block, which
    // a forward-lit translucent fragment reads too. Matches deferred_lighting.frag PushConstants
    // byte-for-byte.
    struct LightingPushConstants
    {
        u32 AlbedoTexture;
        u32 NormalTexture;
        u32 OrmTexture;
        u32 DepthTexture;
        u32 EmissiveTexture;
        u32 Sampler;
        u32 ViewConstantsIndex;
    };

    // The SSAO-enabled lighting variant's push block: the base fields plus the AO bindless slot.
    // Matches deferred_lighting_ssao.frag PushConstants byte-for-byte.
    struct SsaoLightingPushConstants
    {
        u32 AlbedoTexture;
        u32 NormalTexture;
        u32 OrmTexture;
        u32 DepthTexture;
        u32 EmissiveTexture;
        u32 Sampler;
        u32 ViewConstantsIndex;
        u32 SsaoTexture;
    };

    /// @brief The fullscreen deferred-lighting pass.
    ///
    /// Evaluates the shared lighting core (Veng/lighting.slang) over the g-buffer into the HDR (or,
    /// for the cascade-debug terminal arm, the output) target, reading the view's light state from
    /// the view block. Declaring .Sample on each g-buffer id drives the
    /// graph-derived attachment → shader-read transitions, including the depth attachment →
    /// shader-read barrier.
    class DeferredLightingScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context          Renderer context for bindless access.
        /// @param pipeline         The lighting pipeline (plain or SSAO variant).
        /// @param extent           Initial render extent; updated via Resize.
        /// @param useSsao          When true, selects the SSAO-enabled pipeline and push block,
        ///                         and also samples io.Ssao.
        /// @param shadowSet        The dedicated set-3 descriptor set (atlas + comparison sampler
        ///                         + ShadowConstants ring); always valid — the renderer keeps a
        ///                         dummy atlas bound when shadows are off.
        /// @param shadowRingStride Per-frame ShadowConstants region stride; the pass selects the
        ///                         current region via a bind-time dynamic offset.
        /// @param punctualRingStride Per-frame PunctualShadow region stride.
        /// @param iblSet           The set-4 IBL maps + sampler descriptor set (always valid).
        /// @param writeToOutput    When true, writes directly to the output target (cascade-debug
        ///                         terminal arm); otherwise writes the HDR target.
        DeferredLightingScenePass(Context& context, Ref<GraphicsPipeline> pipeline, uvec2 extent,
                                  bool useSsao, Ref<DescriptorSet> shadowSet, u32 shadowRingStride,
                                  u32 punctualRingStride, Ref<DescriptorSet> iblSet,
                                  bool writeToOutput = false)
            : m_Context(context), m_Pipeline(std::move(pipeline)), m_Extent(extent),
              m_UseSsao(useSsao), m_ShadowSet(std::move(shadowSet)),
              m_ShadowRingStride(shadowRingStride), m_PunctualRingStride(punctualRingStride),
              m_IblSet(std::move(iblSet)), m_WriteToOutput(writeToOutput)
        {
        }

        /// @brief Updates the render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the deferred-lighting pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The lighting pipeline (plain or SSAO variant).
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief Current render extent.
        uvec2 m_Extent;
        /// @brief Whether the SSAO-enabled variant is active.
        bool m_UseSsao = false;
        /// @brief The set-3 shadow descriptor set.
        Ref<DescriptorSet> m_ShadowSet;
        /// @brief Per-frame ShadowConstants region stride.
        u32 m_ShadowRingStride = 0;
        /// @brief Per-frame PunctualShadow region stride.
        u32 m_PunctualRingStride = 0;
        /// @brief The set-4 IBL descriptor set.
        Ref<DescriptorSet> m_IblSet;
        /// @brief Whether this pass writes directly to the output target.
        bool m_WriteToOutput = false;
    };
}
