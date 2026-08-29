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
    class GraphicsPipeline;

    /// @brief The forward translucent pass.
    ///
    /// Draws the gathered translucent submeshes back-to-front into the lit HDR scene-color
    /// target after deferred lighting and the sky composite and before the bloom/tonemap tail,
    /// so translucents bloom and tonemap with the scene. Depth-TESTs against the opaque depth
    /// buffer with depth writes OFF, and STRAIGHT-alpha-blends each fragment's returned final
    /// HDR color. Each translucent material's pipeline is built per parent (against the HDR
    /// format, which the material loader does not know) and cached here; a draw binds its
    /// parent's pipeline, the set-0 bindless registry, and the shared set-1 DrawData SSBO, then
    /// reads its per-draw record and material selector from DrawData by the instance-rate
    /// candidate id, exactly like a static surface draw.
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
        TranslucentScenePass(Context& context, uvec2 extent, const TranslucentDrawPlan* plan,
                             ResourceId targetId, ResourceId depthId, ResourceId sceneColorId,
                             ResourceId sceneDepthId, Format targetFormat, ResourceId maskId,
                             Format maskFormat, bool halfResolution)
            : m_Context(context), m_Extent(extent), m_Plan(plan), m_TargetId(targetId),
              m_DepthId(depthId), m_SceneColorId(sceneColorId), m_SceneDepthId(sceneDepthId),
              m_TargetFormat(targetFormat), m_MaskId(maskId), m_MaskFormat(maskFormat),
              m_HalfResolution(halfResolution)
        {
        }

        /// @brief Updates the render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the translucent pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Returns the per-parent alpha-blend pipeline, building and caching it on first use.
        const Ref<GraphicsPipeline>& PipelineFor(const MaterialInstance& material) const;

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
        // Per-parent pipeline cache; mutable so PipelineFor can lazily populate it from the
        // const record callback.
        mutable std::unordered_map<const Material*, Ref<GraphicsPipeline>> m_Pipelines;
    };
}
