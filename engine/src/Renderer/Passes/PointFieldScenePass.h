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

    /// @brief Draws the scene's point fields into the LDR scene color after tonemap, LOD-culled.
    ///
    /// Runs at the full output extent, compositing over the tonemapped image with LoadOp::Load —
    /// a point field is an unlit emissive primitive, so it draws after the deferred chain like the
    /// DebugDraw pass, not through the g-buffer. Each Execute the pass draws every field the
    /// renderer resolved this frame: per field it CPU-frustum-culls the field's spatial cells
    /// against the camera, then per surviving cell estimates on-screen point density and routes the
    /// cell to one of two draws: the resolved sprites (camera-facing quads expanded from the cell's
    /// point range, additive-blended) below the density threshold, or a single additive density
    /// splat above it. Both paths pull points from the field's resident SSBO (set 1 binding 0) by
    /// SV_VertexID — no vertex input, no per-instance attribute — so the draw stays MoltenVK-clean
    /// (no drawIndirectCount, no base-instance capability).
    ///
    /// The fields are borrowed from the scene's PointField components (the renderer refills the set
    /// each Execute); an empty set makes the pass a per-frame no-op. Inserted only while a live
    /// field exists, so the shipping deferred path and the smoke golden are unchanged.
    class PointFieldScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass, building the sprite and aggregate pipelines.
        /// @param context        The render context.
        /// @param assets         Asset manager the point-field shaders load through (the core pack).
        /// @param fields         The renderer-owned live field set this Execute, refilled per frame.
        /// @param outputFormat   Color format of the output target the pass writes.
        /// @param samplerHandle  Shared sampler bindless handle for the depth sample.
        /// @param framesInFlight Number of frame-in-flight ring regions for the aggregate records.
        /// @param extent         Initial output extent; updated via Resize.
        PointFieldScenePass(Context& context, AssetManager& assets,
                            const vector<const PointField*>* fields, Format outputFormat,
                            SamplerHandle samplerHandle, u32 framesInFlight, uvec2 extent);

        /// @brief Destroys the pass's owned GPU resources.
        ~PointFieldScenePass() override;

        /// @brief Updates the cached output extent.
        void Resize(uvec2 extent) override;

        /// @brief Contributes the point-field pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Per-field GPU state and hysteresis latch, cached across frames per drawn field.
        ///
        /// Each live field draws through its own descriptor sets and aggregate ring so concurrent
        /// per-frame writes never collide with a sibling field's pending draws in the same command
        /// buffer. Cached keyed by the field pointer (fields are long-lived resources) and pruned
        /// when a field is no longer resolved.
        struct FieldState
        {
            /// @brief Set-1 descriptor pointing at the field's resident point buffer (sprite draw).
            Ref<DescriptorSet> SpriteSet;
            /// @brief The buffer SpriteSet points at, so it is re-pointed only when the field rebuilds.
            const Buffer* BoundBuffer = nullptr;
            /// @brief Set-1 descriptor for this field's aggregate draw (the per-frame splat records).
            Ref<DescriptorSet> AggregateSet;
            /// @brief Host-mapped per-frame aggregate splat-record SSBO, ringed for frames-in-flight.
            Ref<Buffer> AggregateBuffer;
            /// @brief Byte stride between this field's aggregate-buffer ring regions.
            u64 AggregateRegionStride = 0;
            /// @brief Per-cell "currently aggregating" latch, keyed by cell index, for the LOD hysteresis.
            ///
            /// A cell's sprite<->aggregate choice retains across frames so the transition is
            /// hysteretic: a resolving cell flips to aggregate only once its density passes the high
            /// gate, and back only once it falls below the low gate. Sized to the field's cell count.
            vector<bool> Aggregating;
            /// @brief Whether this field was resolved this Execute; unseen entries are pruned.
            bool Seen = false;
        };

        /// @brief Ensures a FieldState exists for @p field, allocating its sets and ring on first use.
        FieldState& StateFor(const PointField* field);

        /// @brief The render context.
        Context& m_Context;
        /// @brief Borrowed pointer to the renderer's live field set, refilled each Execute (may be empty).
        const vector<const PointField*>* m_Fields;
        /// @brief Number of frame-in-flight ring regions each field's aggregate buffer carries.
        u32 m_FramesInFlight;
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

        /// @brief Per-field GPU state and latches, keyed by field pointer, pruned when unresolved.
        unordered_map<const PointField*, FieldState> m_Fields_State;
    };
}
