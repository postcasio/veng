#pragma once

#include <unordered_map>

#include <Veng/Veng.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

#include "../DrawPlan.h"

namespace Veng
{
    class Material;
    class MaterialInstance;
}

namespace Veng::Renderer
{
    class Context;
    class DescriptorSet;
    class DescriptorSetLayout;
    class GraphicsPipeline;
    class PipelineLayout;

    /// @brief The set a forward-lit Translucent fragment reads the image-based-lighting maps at.
    ///
    /// Mirrors Veng/lighting.slang's set-4 IBL declarations; set 3 is the per-draw DrawData.
    inline constexpr u32 ForwardLightingIblSet = 4;

    /// @brief The set a forward-lit Translucent fragment reads the shadow system at.
    ///
    /// Mirrors Veng/forward_lighting.slang's VE_SHADOW_SET.
    inline constexpr u32 ForwardLightingShadowSet = 5;

    /// @brief The renderer-owned sets a forward-lit Translucent fragment is drawn with.
    ///
    /// The same IBL and shadow sets the deferred lighting pass binds, with their layouts, so a
    /// translucent pipeline built against these layouts at ForwardLightingIblSet and
    /// ForwardLightingShadowSet binds them directly.
    struct ForwardLightingSets
    {
        /// @brief The IBL consumer set's layout (radiance, irradiance, prefilter, BRDF LUT, sampler).
        Ref<DescriptorSetLayout> IblLayout;
        /// @brief The IBL consumer set; always valid.
        Ref<DescriptorSet> IblSet;
        /// @brief The shadow system's set layout (atlases, comparison sampler, the two rings).
        Ref<DescriptorSetLayout> ShadowLayout;
        /// @brief The shadow system's set; always valid (a dummy atlas stands in with shadows off).
        Ref<DescriptorSet> ShadowSet;
        /// @brief Per-frame ShadowConstants region stride (the first dynamic offset).
        u32 ShadowRingStride = 0;
        /// @brief Per-frame PunctualShadows region stride (the second dynamic offset).
        u32 PunctualRingStride = 0;
    };

    /// @brief The forward translucent pass.
    ///
    /// Draws the gathered translucent submeshes back-to-front into the lit HDR scene-color
    /// target after deferred lighting and the sky composite and before the bloom/tonemap tail,
    /// so translucents bloom and tonemap with the scene. Depth-TESTs against the opaque depth
    /// buffer with depth writes OFF, and STRAIGHT-alpha-blends each fragment's returned final
    /// HDR color. Each translucent material's pipeline is built per parent (against the HDR
    /// format, which the material loader does not know) and cached here; a draw binds its
    /// parent's pipeline, the set-0 bindless registry, and the shared set-3 DrawData SSBO, then
    /// reads its per-draw record and material selector from DrawData by the instance-rate
    /// candidate id, exactly like a static surface draw.
    ///
    /// A material whose fragment includes Veng/forward_lighting.slang declares bindings at
    /// ForwardLightingIblSet and ForwardLightingShadowSet; its pipeline is built against the
    /// renderer's own layouts at those sets rather than the reflected ones (the shadow set carries
    /// an immutable comparison sampler reflection cannot express), and its draws bind the renderer's
    /// IBL and shadow sets there, so the fragment runs the deferred pass's light loop.
    ///
    /// The pass also owns the bloom mask, a second color attachment it clears at pass begin and a
    /// declaring material writes as SV_Target1 — the glow strength a surface asks for apart from
    /// how bright it is, which the bloom pyramid's level 0 folds in. The attachment is bound for
    /// every pipeline the pass records (a render-pass instance and its pipelines must agree on the
    /// attachment count) and its writes are enabled only on the pipelines whose material declares
    /// the output, so a material returning a bare float4 leaves the mask alone.
    class TranslucentScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context      Renderer context for pipeline and bindless access.
        /// @param extent       Initial render extent; updated via Resize.
        /// @param plan         Borrowed per-frame translucent draw plan (back-to-front).
        /// @param targetId     The lit scene-color target the pass alpha-blends into.
        /// @param depthId      The opaque depth target, bound read-only for depth-testing.
        /// @param sceneColorId Refraction scene-color intermediate, or invalid when off.
        /// @param sceneDepthId Refraction depth intermediate, or invalid when off.
        /// @param targetFormat Color format the per-parent pipelines target.
        /// @param maskId       The bloom-mask target, or invalid when the frame wires none.
        /// @param maskFormat   Color format of the bloom-mask attachment.
        /// @param halfResolution Renders the reduced-resolution layer: the viewport is the half
        ///                     valid sub-rect and the target is cleared to transparent at pass
        ///                     begin (the layer accumulates over nothing and composites later).
        /// @param forward      The IBL and shadow sets a forward-lit material's draws bind.
        TranslucentScenePass(Context& context, uvec2 extent, const TranslucentDrawPlan* plan,
                             ResourceId targetId, ResourceId depthId, ResourceId sceneColorId,
                             ResourceId sceneDepthId, Format targetFormat, ResourceId maskId,
                             Format maskFormat, bool halfResolution, ForwardLightingSets forward)
            : m_Context(context), m_Extent(extent), m_Plan(plan), m_TargetId(targetId),
              m_DepthId(depthId), m_SceneColorId(sceneColorId), m_SceneDepthId(sceneDepthId),
              m_TargetFormat(targetFormat), m_MaskId(maskId), m_MaskFormat(maskFormat),
              m_HalfResolution(halfResolution), m_Forward(std::move(forward))
        {
        }

        /// @brief Whether a material's fragment runs the forward light loop.
        ///
        /// True when its reflected fragment interface declares any binding at
        /// ForwardLightingIblSet or ForwardLightingShadowSet — the sets Veng/forward_lighting.slang
        /// reads.
        [[nodiscard]] static bool IsForwardLit(const Material& material);

        /// @brief Updates the render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the translucent pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief A cached per-parent pipeline and whether its draws bind the forward-lighting sets.
        struct CachedPipeline
        {
            /// @brief The alpha-blend pipeline.
            Ref<GraphicsPipeline> Pipeline;
            /// @brief True when the pipeline's layout carries the forward-lighting sets.
            bool ForwardLit = false;
        };

        /// @brief Returns the per-parent alpha-blend pipeline, building and caching it on first use.
        const CachedPipeline& PipelineFor(const MaterialInstance& material) const;

        /// @brief Builds a forward-lit material's layout: its own DrawData set, then the renderer's
        /// IBL and shadow set layouts, with the material's push ranges.
        Ref<PipelineLayout> ForwardLitLayoutFor(const Material& parent) const;

        /// @brief Records the back-to-front translucent draws into the target.
        void Record(const ScenePassContext& ctx) const;

        /// @brief Renderer context for pipeline and bindless access.
        Context& m_Context;
        /// @brief Current render extent.
        uvec2 m_Extent;
        /// @brief Borrowed per-frame translucent draw plan.
        const TranslucentDrawPlan* m_Plan = nullptr;
        /// @brief The lit scene-color target.
        ResourceId m_TargetId;
        /// @brief The opaque depth target.
        ResourceId m_DepthId;
        /// @brief Refraction scene-color intermediate id (invalid when off).
        ResourceId m_SceneColorId;
        /// @brief Refraction depth intermediate id (invalid when off).
        ResourceId m_SceneDepthId;
        /// @brief Color format the per-parent pipelines target.
        Format m_TargetFormat;
        /// @brief The bloom-mask target (invalid when the frame wires none).
        ResourceId m_MaskId;
        /// @brief Color format of the bloom-mask attachment.
        Format m_MaskFormat;
        /// @brief Whether this instance renders the reduced-resolution layer.
        bool m_HalfResolution;
        /// @brief The IBL and shadow sets a forward-lit material's draws bind.
        ForwardLightingSets m_Forward;
        // Per-parent pipeline cache; mutable so PipelineFor can lazily populate it from the
        // const record callback.
        mutable std::unordered_map<const Material*, CachedPipeline> m_Pipelines;
    };
}
