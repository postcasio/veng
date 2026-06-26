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
}
