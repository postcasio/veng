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

    // The deferred-lighting fragment push block: g-buffer bindless slots (including the G4
    // emissive read), view-constants index, and light buffer base + live count.
    // Matches deferred_lighting.frag PushConstants byte-for-byte.
    struct LightingPushConstants
    {
        u32 AlbedoTexture;
        u32 NormalTexture;
        u32 OrmTexture;
        u32 DepthTexture;
        u32 EmissiveTexture;
        u32 Sampler;
        u32 ViewConstantsIndex;
        u32 LightBase;
        u32 LightCount;
        u32 IblEnabled;        // 1 = sample the IBL set's maps; 0 = flat ambient fallback
        u32 SkylightOn;        // 1 = SH skylight ambient (second arm); 0 = flat ambient fallback
        u32 PrefilterMipCount; // prefiltered specular mip count (roughness → LOD)
        f32 EnvIntensity;      // scales the IBL ambient
        f32 SkylightIntensity; // scales the SH skylight ambient
        u32 LtcMatTexture;     // LTC inverse-matrix LUT bindless slot (area lights)
        u32 LtcMagTexture;     // LTC magnitude/Fresnel LUT bindless slot (area lights)
        u32 AreaVertexBase;    // current frame's base index into the area-vertex buffer
    };

    // The SSAO-enabled lighting variant's push block: the base + IBL fields plus the AO
    // bindless slot. Matches deferred_lighting_ssao.frag PushConstants byte-for-byte
    // (sixteen u32s — one pad reaches a 16-byte boundary).
    struct SsaoLightingPushConstants
    {
        u32 AlbedoTexture;
        u32 NormalTexture;
        u32 OrmTexture;
        u32 DepthTexture;
        u32 EmissiveTexture;
        u32 Sampler;
        u32 ViewConstantsIndex;
        u32 LightBase;
        u32 LightCount;
        u32 IblEnabled;
        u32 SkylightOn;
        u32 PrefilterMipCount;
        f32 EnvIntensity;
        f32 SkylightIntensity;
        u32 LtcMatTexture;
        u32 LtcMagTexture;
        u32 AreaVertexBase;
        u32 SsaoTexture;
        u32 Pad1;
    };

    /// @brief The fullscreen deferred-lighting pass.
    ///
    /// Evaluates Cook-Torrance lighting over the g-buffer into the HDR (or, for the cascade-debug
    /// terminal arm, the output) target. Declaring .Sample on each g-buffer id drives the
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
        /// @param shadowSet        The dedicated set-1 descriptor set (atlas + comparison sampler
        ///                         + ShadowConstants ring); always valid — the renderer keeps a
        ///                         dummy atlas bound when shadows are off.
        /// @param shadowRingStride Per-frame ShadowConstants region stride; the pass selects the
        ///                         current region via a bind-time dynamic offset.
        /// @param punctualRingStride Per-frame PunctualShadow region stride.
        /// @param iblSet           The set-2 IBL maps + sampler descriptor set (always valid).
        /// @param prefilterMipCount Prefiltered specular mip count (roughness → LOD).
        /// @param skylight         Whether the SH skylight ambient arm is enabled.
        /// @param iblAllowed       Whether the resolved sky requests the IBL tier.
        /// @param writeToOutput    When true, writes directly to the output target (cascade-debug
        ///                         terminal arm); otherwise writes the HDR target.
        DeferredLightingScenePass(Context& context, Ref<GraphicsPipeline> pipeline, uvec2 extent,
                                  bool useSsao, Ref<DescriptorSet> shadowSet, u32 shadowRingStride,
                                  u32 punctualRingStride, Ref<DescriptorSet> iblSet,
                                  u32 prefilterMipCount, bool skylight, bool iblAllowed,
                                  bool writeToOutput = false)
            : m_Context(context), m_Pipeline(std::move(pipeline)), m_Extent(extent),
              m_UseSsao(useSsao), m_ShadowSet(std::move(shadowSet)),
              m_ShadowRingStride(shadowRingStride), m_PunctualRingStride(punctualRingStride),
              m_IblSet(std::move(iblSet)), m_PrefilterMipCount(prefilterMipCount),
              m_Skylight(skylight), m_IblAllowed(iblAllowed), m_WriteToOutput(writeToOutput)
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
        /// @brief The set-1 shadow descriptor set.
        Ref<DescriptorSet> m_ShadowSet;
        /// @brief Per-frame ShadowConstants region stride.
        u32 m_ShadowRingStride = 0;
        /// @brief Per-frame PunctualShadow region stride.
        u32 m_PunctualRingStride = 0;
        /// @brief The set-2 IBL descriptor set.
        Ref<DescriptorSet> m_IblSet;
        /// @brief Prefiltered specular mip count.
        u32 m_PrefilterMipCount = 0;
        /// @brief Whether the SH skylight ambient arm is enabled.
        bool m_Skylight = false;
        /// @brief Whether the resolved sky requests the IBL tier.
        bool m_IblAllowed = true;
        /// @brief Whether this pass writes directly to the output target.
        bool m_WriteToOutput = false;
    };
}
