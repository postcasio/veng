#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;
    class PipelineLayout;
    class DescriptorSet;
    class DescriptorSetLayout;
    class Buffer;
    class PointField;

    /// @brief Draws a PointField into the LDR scene color after tonemap, frustum-culled with a distance LOD.
    ///
    /// Runs at the full output extent, compositing over the tonemapped image with LoadOp::Load —
    /// a point field is an unlit emissive primitive, so it draws after the deferred chain like the
    /// DebugDraw pass, not through the g-buffer. Each Execute the pass CPU-frustum-culls the
    /// field's spatial cells against the camera, then per surviving cell estimates on-screen point
    /// density and routes the cell to one of two draws: the resolved sprites (camera-facing quads
    /// expanded from the cell's point range, additive-blended) below the density threshold, or a
    /// single additive density splat above it. Both paths pull points from the field's resident
    /// SSBO (set 1 binding 0) by SV_VertexID — no vertex input, no per-instance attribute — so the
    /// draw stays MoltenVK-clean (no drawIndirectCount, no base-instance capability).
    ///
    /// The field is borrowed (SceneRenderer::SetPointField); a null or empty field makes the pass
    /// a per-frame no-op. Allocated only when SceneRendererSettings::PointField is set, so the
    /// shipping deferred path and the smoke golden are unchanged.
    class PointFieldScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass, building the sprite and aggregate pipelines.
        /// @param context        The render context.
        /// @param assets         Asset manager the point-field shaders load through (the core pack).
        /// @param field          The renderer-owned borrowed pointer to the field to draw (may be null).
        /// @param outputFormat   Color format of the output target the pass writes.
        /// @param samplerHandle  Shared sampler bindless handle for the depth sample.
        /// @param framesInFlight Number of frame-in-flight ring regions for the aggregate records.
        /// @param extent         Initial output extent; updated via Resize.
        PointFieldScenePass(Context& context, AssetManager& assets, const PointField* const* field,
                            Format outputFormat, SamplerHandle samplerHandle, u32 framesInFlight,
                            uvec2 extent);

        /// @brief Destroys the pass's owned GPU resources.
        ~PointFieldScenePass() override;

        /// @brief Updates the cached output extent.
        void Resize(uvec2 extent) override;

        /// @brief Contributes the point-field pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief The render context.
        Context& m_Context;
        /// @brief Borrowed pointer-to-pointer to the field the renderer holds (may point to null).
        const PointField* const* m_Field;
        /// @brief Output color format the pipelines target.
        Format m_OutputFormat;
        /// @brief Shared sampler bindless handle for the depth sample.
        SamplerHandle m_SamplerHandle;
        /// @brief Current output extent.
        uvec2 m_Extent;

        /// @brief Sprite pipeline (camera-facing quad expansion, additive blend) and its layout.
        Ref<GraphicsPipeline> m_SpritePipeline;
        /// @brief Aggregate pipeline (per-cell additive density splat) and its layout.
        Ref<GraphicsPipeline> m_AggregatePipeline;
        /// @brief Shared layout for both pipelines (set 0 bindless + set 1 point SSBO + push block).
        Ref<PipelineLayout> m_Layout;
        /// @brief Set-1 layout for the resident point SSBO (binding 0 storage buffer).
        Ref<DescriptorSetLayout> m_SetLayout;
        /// @brief Descriptor set bound at set 1 pointing at the field's resident point buffer.
        Ref<DescriptorSet> m_Set;
        /// @brief The buffer m_Set currently points at, so it is re-pointed only when the field changes.
        const Buffer* m_BoundBuffer = nullptr;

        /// @brief Descriptor set bound at set 1 for the aggregate draw (the per-frame splat records).
        Ref<DescriptorSet> m_AggregateSet;
        /// @brief Host-mapped per-frame aggregate splat-record SSBO, ring-buffered for frames-in-flight.
        Ref<Buffer> m_AggregateBuffer;
        /// @brief Byte stride between aggregate-buffer ring regions.
        u64 m_AggregateRegionStride = 0;

        /// @brief Per-cell "currently aggregating" latch, keyed by cell index, for the LOD hysteresis.
        ///
        /// A cell's sprite<->aggregate choice retains across frames so the transition is hysteretic:
        /// a resolving cell flips to aggregate only once its density passes the high gate, and back
        /// only once it falls below the low gate. Rebuilt against the bound field's cell count when
        /// the field changes (so a swapped field starts with a clean latch).
        vector<bool> m_Aggregating;
        /// @brief The field pointer m_Aggregating was sized against, so the latch resets on a field swap.
        const PointField* m_LatchedField = nullptr;
    };
}
