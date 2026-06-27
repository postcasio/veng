#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;
    class PipelineLayout;
    class DescriptorSet;
    class DescriptorSetLayout;
    class Buffer;
    class DebugDraw;

    /// @brief Flushes a SceneRenderer's DebugDraw accumulator into the LDR scene color after tonemap.
    ///
    /// Runs at the full output extent, writing the output target with LoadOp::Load (composites
    /// over the tonemapped image). It is not hardware depth-tested; instead both the line and
    /// billboard fragment shaders sample the g-buffer depth — remapping the logical UV by the
    /// tonemap pass's RenderScaleUV/MaxValidUV so the sample is correct under dynamic resolution —
    /// and fade an occluded fragment rather than hiding it (the single-pass occluded fallback).
    ///
    /// Lines are uploaded into a per-frame-in-flight ring region of a host-mapped vertex buffer
    /// and rasterized as a LineList; billboards are uploaded into a per-frame-in-flight ring region
    /// of a host-mapped SSBO (set 1 binding 0) and drawn instanced (six vertices per record). The
    /// pass reads the renderer-owned DebugDraw accumulator each frame through a stored pointer; the
    /// accumulator clears at the start of each Execute, so a primitive is re-pushed each frame.
    class DebugDrawScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass, building the line and billboard pipelines.
        /// @param context        The render context.
        /// @param assets         Asset manager the debug shaders load through (the core pack).
        /// @param accumulator    The renderer-owned accumulator this pass flushes each frame.
        /// @param outputFormat   Color format of the output target the pass writes.
        /// @param samplerHandle  Shared sampler bindless handle for the depth/icon samples.
        /// @param framesInFlight Number of frame-in-flight ring regions to size the buffers for.
        /// @param extent         Initial output extent; updated via Resize.
        DebugDrawScenePass(Context& context, AssetManager& assets, const DebugDraw* accumulator,
                           Format outputFormat, SamplerHandle samplerHandle, u32 framesInFlight,
                           uvec2 extent);

        /// @brief Destroys the pass's owned GPU resources.
        ~DebugDrawScenePass() override;

        /// @brief Updates the cached output extent.
        void Resize(uvec2 extent) override;

        /// @brief Contributes the debug-draw pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Uploads this frame's line vertices into the current ring region and binds them.
        ///
        /// Returns the number of line vertices written (0 when there are none).
        u32 UploadLines();

        /// @brief Uploads this frame's billboard records into the current ring region and writes the set.
        ///
        /// Returns the number of billboard records written (0 when there are none).
        u32 UploadBillboards();

        /// @brief The render context.
        Context& m_Context;
        /// @brief The renderer-owned accumulator flushed each frame (borrowed).
        const DebugDraw* m_Accumulator;
        /// @brief Output color format the pipelines target.
        Format m_OutputFormat;
        /// @brief Shared sampler bindless handle for the depth/icon samples.
        SamplerHandle m_SamplerHandle;
        /// @brief Number of frame-in-flight ring regions.
        u32 m_FramesInFlight;
        /// @brief Current output extent.
        uvec2 m_Extent;

        /// @brief Line pipeline (LineList topology, alpha blend) and its layout.
        Ref<GraphicsPipeline> m_LinePipeline;
        /// @brief Layout for the line pipeline (set 0 bindless + push block).
        Ref<PipelineLayout> m_LineLayout;

        /// @brief Billboard pipeline (instanced quad, alpha blend) and its layout.
        Ref<GraphicsPipeline> m_BillboardPipeline;
        /// @brief Layout for the billboard pipeline (set 0 bindless + set 1 record SSBO + push block).
        Ref<PipelineLayout> m_BillboardLayout;
        /// @brief Set-1 layout for the billboard records (binding 0 storage buffer).
        Ref<DescriptorSetLayout> m_BillboardSetLayout;
        /// @brief Descriptor set bound at set 1 for the billboard draw (whole-range, ringed by offset).
        Ref<DescriptorSet> m_BillboardSet;

        /// @brief Host-mapped line vertex buffer, ring-buffered for frames-in-flight.
        Ref<Buffer> m_LineBuffer;
        /// @brief Byte stride between line-buffer ring regions.
        u64 m_LineRegionStride = 0;

        /// @brief Host-mapped billboard record SSBO, ring-buffered for frames-in-flight.
        Ref<Buffer> m_BillboardBuffer;
        /// @brief Byte stride between billboard-buffer ring regions.
        u64 m_BillboardRegionStride = 0;
    };

    /// @brief Writes each pickable billboard's entity pick id into the EntityId target, depth-discarded.
    ///
    /// The id-only sibling of DebugDrawScenePass's decorative billboard render: it runs in the
    /// geometry-pass timeframe (while the SceneRenderer's EntityId target is still bound, before
    /// lighting), not after tonemap, so the id is written into the picking target the async readback
    /// copies from. For each accumulated billboard carrying a non-zero PickId it rasterizes a fixed
    /// min-size proxy footprint (a centered disc clamped to a minimum pixel radius — not the icon's
    /// art alpha) and writes that id. Occlusion is a hard hardware depth test against the picking
    /// depth the mesh id pass wrote: an occluded fragment is discarded (not faded), so an icon behind
    /// geometry is not picked and an icon in front wins. It depth-tests but never writes depth, so two
    /// overlapping icons both reach the id target (the nearest wins by the readback's depth precedence).
    ///
    /// Allocated and wired only when SceneRendererSettings::Picking is set; a decorative-only viewport
    /// (DebugDraw without Picking) never builds it, so the shipping path is unchanged.
    class BillboardPickScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass, building the proxy id-write pipeline.
        /// @param context        The render context.
        /// @param assets         Asset manager the picking billboard shaders load through.
        /// @param accumulator    The renderer-owned accumulator this pass reads pickable billboards from.
        /// @param depthFormat    Format of the picking depth buffer the pass depth-tests against.
        /// @param framesInFlight Number of frame-in-flight ring regions to size the record buffer for.
        /// @param extent         Initial render extent; updated via Resize.
        /// @param entityIdId     The imported EntityId color target the pass writes pick ids into.
        /// @param depthId        The imported picking depth target the pass depth-tests against.
        BillboardPickScenePass(Context& context, AssetManager& assets, const DebugDraw* accumulator,
                               Format depthFormat, u32 framesInFlight, uvec2 extent,
                               ResourceId entityIdId, ResourceId depthId);

        /// @brief Destroys the pass's owned GPU resources.
        ~BillboardPickScenePass() override;

        /// @brief Updates the cached render extent.
        void Resize(uvec2 extent) override;

        /// @brief Contributes the billboard id-write pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Uploads this frame's pickable billboard records into the current ring region.
        ///
        /// Filters the accumulator to records with a non-zero PickId, writes them into the current
        /// frame-in-flight region, binds it as the SSBO range, and returns the count (0 when none).
        u32 UploadPickableBillboards();

        /// @brief The render context.
        Context& m_Context;
        /// @brief The renderer-owned accumulator the pickable billboards are read from (borrowed).
        const DebugDraw* m_Accumulator;
        /// @brief Number of frame-in-flight ring regions.
        u32 m_FramesInFlight;
        /// @brief Current render extent (the picking sub-rect the proxy footprint is sized against).
        uvec2 m_Extent;
        /// @brief The imported EntityId color target this pass writes into.
        ResourceId m_EntityIdId;
        /// @brief The imported picking depth target this pass depth-tests against.
        ResourceId m_DepthId;

        /// @brief Proxy id-write pipeline (depth-test on, no depth-write, no blend, R32Uint target).
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief Layout for the pipeline (set 1 record SSBO + push block).
        Ref<PipelineLayout> m_Layout;
        /// @brief Set-1 layout for the billboard pick records (binding 0 storage buffer).
        Ref<DescriptorSetLayout> m_SetLayout;
        /// @brief Descriptor set bound at set 1 for the draw (whole-range, ringed by offset).
        Ref<DescriptorSet> m_Set;

        /// @brief Host-mapped billboard pick-record SSBO, ring-buffered for frames-in-flight.
        Ref<Buffer> m_Buffer;
        /// @brief Byte stride between record-buffer ring regions.
        u64 m_RegionStride = 0;
    };
}
