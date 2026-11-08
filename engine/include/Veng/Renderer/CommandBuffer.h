#pragma once

#include <initializer_list>
#include <span>

#include <Veng/Assert.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    template <typename V>
    class VertexBuffer;
    class IndexBuffer;
    class Context;

    /// @brief Raw, untyped push-constants descriptor for exotic or dynamic use
    /// (e.g. a single push spanning ranges with different stages).
    ///
    /// The layout comes from the command buffer's last-bound pipeline layout. Prefer
    /// CommandBuffer::PushConstants<T>, which derives StageFlags and Size from the
    /// declared PushConstantRange.
    struct PushConstantsInfo
    {
        /// @brief Shader stages that receive the data.
        ShaderStage StageFlags;
        /// @brief Byte offset within the push-constant block.
        u32 Offset;
        /// @brief Byte size of the data.
        u32 Size;
        /// @brief Pointer to the data to push.
        const void* Data;
    };

    /// @brief Describes a single color, depth, or stencil attachment for a dynamic render pass.
    struct RenderingAttachmentInfo
    {
        /// @brief The view to attach; null leaves a color slot unbound (its writes are discarded,
        ///        and a pipeline drawn in the pass declares Format::Undefined there).
        Ref<ImageView> ImageView;
        /// @brief Load operation at pass begin.
        LoadOp LoadOp;
        /// @brief Store operation at pass end.
        StoreOp StoreOp;
        /// @brief Clear value used when LoadOp == Clear.
        ClearValue ClearValue;
    };

    /// @brief Parameters for beginning a dynamic render pass via BeginRendering.
    struct RenderingInfo
    {
        /// @brief Render area offset.
        ivec2 Offset = {0, 0};
        /// @brief Render area extent.
        uvec2 Extent;
        /// @brief Number of layers to render into.
        u32 LayerCount = 1;
        /// @brief Multiview mask (0 = disabled).
        u32 ViewMask = 0;
        /// @brief Color attachment list.
        vector<RenderingAttachmentInfo> ColorAttachments;
        /// @brief Optional depth attachment.
        optional<RenderingAttachmentInfo> DepthAttachment;
        /// @brief Optional stencil attachment.
        optional<RenderingAttachmentInfo> StencilAttachment;
    };

    /// @brief Parameters for binding descriptor sets, with optional dynamic offsets.
    struct DescriptorSetBindInfo
    {
        /// @brief Sets to bind.
        vector<Ref<DescriptorSet>> Sets;
        /// @brief Index of the first set in the pipeline layout.
        u32 FirstSet = 0;
        /// @brief Bind point.
        PipelineBindPoint PipelineBindPoint = PipelineBindPoint::Graphics;

        /// @brief One byte offset per UniformBufferDynamic / StorageBufferDynamic descriptor
        /// across the bound sets, in binding order — selects the live region of a ring-buffered
        /// dynamic buffer at bind. Empty for sets with no dynamic descriptors.
        vector<u32> DynamicOffsets;
    };

    /// @brief One buffer↔image copy region targeting a rectangle of a single mip level.
    ///
    /// The buffer-side pixels for the region begin at BufferOffset and are tightly packed (no row
    /// padding, rows the region's width); Extent is the region's texel dimensions and ImageOffset
    /// its top-left texel on the image side. The default ImageOffset covers a whole mip level, so a
    /// precooked mip chain uploads (or reads back) from one staging buffer in a single command; a
    /// non-zero ImageOffset targets a sub-rectangle, as a dynamic atlas packing a tile does.
    struct BufferImageCopyRegion
    {
        /// @brief Byte offset of this region's pixels within the buffer.
        u64 BufferOffset = 0;
        /// @brief Destination (or source) mip level.
        u32 MipLevel = 0;
        /// @brief The first array layer the region covers.
        u32 BaseArrayLayer = 0;
        /// @brief The number of array layers the region covers; zero for every layer from
        ///        BaseArrayLayer to the image's last.
        u32 ArrayLayers = 0;
        /// @brief The region's top-left texel on the image side (x, y, z).
        uvec3 ImageOffset = {0, 0, 0};
        /// @brief The region's texel dimensions (width, height, depth).
        uvec3 Extent = {1, 1, 1};
    };

    /// @brief A run of array layers copied between two images by CommandBuffer::CopyImageLayers.
    struct ImageLayerCopy
    {
        /// @brief The first source layer.
        u32 SourceLayer = 0;
        /// @brief The first destination layer.
        u32 DestinationLayer = 0;
        /// @brief The number of layers copied.
        u32 LayerCount = 1;
    };

    /// @brief One region copied between two images by CommandBuffer::CopyImageRegions.
    struct ImageCopyRegion
    {
        /// @brief The source mip level.
        u32 SourceMipLevel = 0;
        /// @brief The first source array layer.
        u32 SourceBaseLayer = 0;
        /// @brief The destination mip level.
        u32 DestinationMipLevel = 0;
        /// @brief The first destination array layer.
        u32 DestinationBaseLayer = 0;
        /// @brief The number of array layers copied.
        u32 LayerCount = 1;
        /// @brief The region's first texel in the source (x, y, z).
        uvec3 SourceOffset = {0, 0, 0};
        /// @brief The region's first texel in the destination (x, y, z).
        uvec3 DestinationOffset = {0, 0, 0};
        /// @brief The region's texel dimensions (width, height, depth).
        uvec3 Extent = {1, 1, 1};
    };

    /// @brief Parameters for a blit between two images, with explicit mip levels and offsets.
    struct BlitImageInfo
    {
        /// @brief Source image.
        Ref<Image> SourceImage;
        /// @brief Destination image.
        Ref<Image> DestinationImage;
        /// @brief Mip level to read from.
        u32 SourceMipLevel;
        /// @brief Mip level to write into.
        u32 DestinationMipLevel;
        /// @brief Source region origin.
        ivec3 SourceOffset;
        /// @brief Destination region origin.
        ivec3 DestinationOffset;
        /// @brief Source region size.
        ivec3 SourceExtent;
        /// @brief Destination region size.
        ivec3 DestinationExtent;
        /// @brief First source array layer.
        u32 SourceBaseLayer = 0;
        /// @brief First destination array layer.
        u32 DestinationBaseLayer = 0;
        /// @brief Number of array layers blitted, each source layer into its destination layer.
        u32 LayerCount = 1;
    };

    /// @brief Records GPU commands on behalf of the render graph and direct callers.
    class VE_API CommandBuffer
    {
    public:
        /// @brief Creates a command buffer at the given level.
        /// @param context The owning context.
        /// @param level   Primary or secondary (default Primary).
        static Ref<CommandBuffer> Create(Context& context,
                                         CommandBufferLevel level = CommandBufferLevel::Primary)
        {
            return Ref<CommandBuffer>(new CommandBuffer(context, level));
        }

        /// @brief Frees the command buffer back to its pool.
        ~CommandBuffer();

        CommandBuffer(const CommandBuffer&) = delete;
        CommandBuffer& operator=(const CommandBuffer&) = delete;

        /// @brief Binds a raw buffer as the vertex buffer.
        void BindVertexBuffer(const Ref<Buffer>& buffer);

        /// @brief Binds a raw buffer as the per-instance vertex buffer (binding 1).
        ///
        /// The binding a pipeline built with GraphicsPipelineInfo::InstanceCandidateId reads one
        /// uint per instance from, at the draw's firstInstance + instance.
        void BindInstanceBuffer(const Ref<Buffer>& buffer);

        /// @brief Binds a raw buffer as the index buffer.
        /// @param buffer The index buffer.
        /// @param type   Index element type (default U32).
        void BindIndexBuffer(const Ref<Buffer>& buffer, IndexType type = IndexType::U32);

        /// @brief Binds a typed vertex buffer. Usage flags and stride are derived from VertexBuffer<V>.
        template <typename V>
        void BindVertexBuffer(const VertexBuffer<V>& buffer);

        /// @brief Binds a typed index buffer. Index width is derived from IndexBuffer.
        void BindIndexBuffer(const IndexBuffer& buffer);

        /// @brief Opaque backend handle; defined in the matching .cpp.
        struct Native;
        /// @brief Returns the backend handle. Mutable ref from a const method by design — see Native.h.
        [[nodiscard]] Native& GetNative() const;

        /// @brief Resets the command buffer, discarding any previously recorded commands.
        void Reset();

        /// @brief Begins recording commands into this buffer.
        /// @param flags Usage hint flags (default None).
        void Begin(CommandBufferUsage flags = CommandBufferUsage::None);

        /// @brief Ends recording.
        void End();

        /// @brief Begins a dynamic render pass.
        /// @param info Attachment configuration and render area.
        void BeginRendering(const RenderingInfo& info);

        /// @brief Ends the active dynamic render pass.
        void EndRendering();

        /// @brief Records a raw (untyped) push-constants update.
        /// @param info Stage flags, offset, size, and data pointer.
        void PushConstants(const PushConstantsInfo& info);

        /// @brief Typed push-constants update. Stages and size come from the PushConstantRange
        /// declared on the bound pipeline layout that contains [offset, offset + sizeof(T));
        /// only the value (and an optional offset for partial or multi-range pushes) come from
        /// the call site.
        /// @tparam T   The push-constant value type; must be <= 128 bytes.
        /// @param value The value to push.
        /// @param offset Byte offset into the push-constant block (default 0).
        template <typename T>
        void PushConstants(const T& value, u32 offset = 0);

        /// @brief Records a non-indexed draw.
        void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance);

        /// @brief Records an indexed draw.
        void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset,
                         u32 firstInstance);

        /// @brief Issues drawCount indexed draws from GPU-resident DrawIndexedIndirectCommand records.
        ///
        /// Each record is read when the draw executes, so a compute pass earlier in the frame may
        /// write it. A command with InstanceCount == 0 executes as a no-op — the compute cull
        /// zeroes culled candidates rather than compacting, since a count-driven indirect draw is
        /// unavailable on MoltenVK (the buffer holds a fixed candidate maximum, one slot per
        /// candidate).
        /// @param buffer    Buffer holding the DrawIndexedIndirectCommand records.
        /// @param offset    4-byte-aligned byte offset of the first command.
        /// @param drawCount Number of command records to issue.
        /// @param stride    Byte stride between records (sizeof(DrawIndexedIndirectCommand) = 20).
        /// @pre buffer was created with BufferUsage::Indirect and is ordered for an indirect read
        ///      (AccessKind::IndirectRead).
        /// @pre A command with a non-zero firstInstance requires the drawIndirectFirstInstance
        ///      device feature.
        void DrawIndexedIndirect(const Ref<Buffer>& buffer, u64 offset, u32 drawCount, u32 stride);

        /// @brief Issues drawCount non-indexed draws from GPU-resident DrawIndirectCommand records.
        ///
        /// Each record is a DrawIndirectCommand — four u32: VertexCount, InstanceCount,
        /// FirstVertex, FirstInstance — read when the draw executes, so a compute pass earlier in
        /// the frame may write them. A record with a vertexCount or instanceCount of 0 draws
        /// nothing. A vertex-pulled primitive list reaches the draw this way without an index buffer
        /// built only to feed DrawIndexedIndirect.
        /// @param buffer    Buffer holding the DrawIndirectCommand records.
        /// @param offset    4-byte-aligned byte offset of the first record.
        /// @param drawCount Number of records to issue; above 1 requires multiDrawIndirect.
        /// @param stride    Byte stride between records (at least sizeof(DrawIndirectCommand) = 16).
        /// @pre buffer was created with BufferUsage::Indirect, and the pass drawing reads it through
        ///      RenderGraph::PassBuilder::IndirectRead, which orders the read after the writes that
        ///      produced it.
        /// @pre A record with a non-zero firstInstance requires the drawIndirectFirstInstance
        ///      device feature.
        void DrawIndirect(const Ref<Buffer>& buffer, u64 offset, u32 drawCount, u32 stride);

        /// @brief Records a compute dispatch.
        void Dispatch(u32 groupsX, u32 groupsY, u32 groupsZ);

        /// @brief Records a compute dispatch whose group counts are read from a GPU buffer.
        ///
        /// The record at @p offset is a DispatchIndirectCommand — three u32 group counts (X, Y,
        /// Z) — read when the dispatch executes, so a compute
        /// pass earlier in the frame may write them. A zero count dispatches nothing. Each count
        /// must not exceed the device's maxComputeWorkGroupCount on its axis.
        /// @param buffer Buffer holding the record.
        /// @param offset 4-byte-aligned byte offset of the record; the record must fit the buffer.
        /// @pre buffer was created with BufferUsage::Indirect, and the pass dispatching reads it
        ///      through RenderGraph::PassBuilder::IndirectRead, which orders the read after the
        ///      writes that produced it.
        void DispatchIndirect(const Ref<Buffer>& buffer, u64 offset = 0);

        /// @brief Binds a graphics pipeline.
        void BindPipeline(const Ref<GraphicsPipeline>& pipeline);

        /// @brief Binds a compute pipeline.
        void BindPipeline(const Ref<ComputePipeline>& pipeline);

        /// @brief Sets the scissor rectangle.
        void SetScissor(ivec2 offset, uvec2 extent);

        /// @brief Sets the blend constant a pipeline blending with BlendFactor::ConstantColor reads.
        /// @param constants  The constant's RGBA.
        void SetBlendConstants(vec4 constants);

        /// @brief Sets the viewport.
        void SetViewport(ivec2 offset, uvec2 extent);

        /// @brief Binds descriptor sets.
        void BindDescriptorSets(const DescriptorSetBindInfo& info);

        /// @brief Binds consecutive descriptor sets without allocating.
        ///
        /// The recording-path form: the sets are borrowed for the call and no container is built,
        /// so a pass binding per pipeline or per tile pays only the backend call.
        /// @param sets           The sets to bind, in set order; at most MaxBoundDescriptorSets.
        /// @param firstSet       Index of the first set in the bound pipeline's layout.
        /// @param bindPoint      Bind point (default Graphics).
        /// @param dynamicOffsets One byte offset per dynamic descriptor across the sets, in binding
        ///                       order; empty for sets with none.
        void BindDescriptorSets(std::span<const DescriptorSet* const> sets, u32 firstSet,
                                PipelineBindPoint bindPoint = PipelineBindPoint::Graphics,
                                std::span<const u32> dynamicOffsets = {});

        /// @brief Binds consecutive descriptor sets from a braced list, without allocating.
        /// @param sets           The sets to bind, in set order; at most MaxBoundDescriptorSets.
        /// @param firstSet       Index of the first set in the bound pipeline's layout.
        /// @param bindPoint      Bind point (default Graphics).
        /// @param dynamicOffsets One byte offset per dynamic descriptor across the sets.
        void BindDescriptorSets(std::initializer_list<const DescriptorSet*> sets, u32 firstSet,
                                PipelineBindPoint bindPoint = PipelineBindPoint::Graphics,
                                std::initializer_list<u32> dynamicOffsets = {})
        {
            BindDescriptorSets(std::span<const DescriptorSet* const>(sets.begin(), sets.size()),
                               firstSet, bindPoint,
                               std::span<const u32>(dynamicOffsets.begin(), dynamicOffsets.size()));
        }

        /// @brief The most descriptor sets one bind call accepts.
        static constexpr u32 MaxBoundDescriptorSets = 8;

        /// @brief Returns the number of draw commands recorded since the last Begin.
        ///
        /// Counts every Draw, DrawIndexed, DrawIndexedIndirect and DrawFullscreenTriangle call — the
        /// commands, not the instances or indirect records they expand to — so it reads how much a
        /// frame's recording cost, which is what batching reduces.
        [[nodiscard]] u32 GetDrawCallCount() const { return m_DrawCallCount; }

        /// @brief Records a fullscreen triangle draw (no vertex buffer needed).
        void DrawFullscreenTriangle();

        /// @brief Copies the contents of a buffer into an image's mip 0 as a single region.
        void CopyBufferToImage(const Ref<Buffer>& buffer, const Ref<Image>& image);

        /// @brief Copies a buffer into an image as one region per supplied mip level.
        ///
        /// Records one copy region per supplied region — each pulling a level's tightly-packed pixels
        /// from BufferOffset in the staging buffer — so a precooked mip chain uploads in a single
        /// command with no GPU mip generation. Every region targets all array layers.
        /// @param buffer  Staging buffer holding every level's pixels, largest-first.
        /// @param image   Destination image, already transitioned to TransferDst.
        /// @param regions One entry per mip level to copy.
        void CopyBufferToImage(const Ref<Buffer>& buffer, const Ref<Image>& image,
                               std::span<const BufferImageCopyRegion> regions);

        /// @brief Copies the contents of an image into a buffer.
        void CopyImageToBuffer(const Ref<Image>& image, const Ref<Buffer>& buffer);

        /// @brief Copies an image into a buffer as one region per supplied mip level.
        ///
        /// The image→buffer mirror of CopyBufferToImage(buffer, image, regions): one
        /// copy region per supplied region, each writing a level's tightly-packed texels across
        /// **every** array layer at BufferOffset, so a whole mip chain reads back into one buffer
        /// in one command rather than one staging buffer per subresource.
        /// @param image   Transfer source; must be in TransferSrc layout.
        /// @param buffer  Transfer destination, large enough for every region.
        /// @param regions One entry per mip level to copy.
        void CopyImageToBuffer(const Ref<Image>& image, const Ref<Buffer>& buffer,
                               std::span<const BufferImageCopyRegion> regions);

        /// @brief Copies a rectangular mip-0 region of an image into a buffer, one tightly-packed region.
        ///
        /// Records a single copy of the @p extent texels at @p offset into the buffer
        /// at byte 0, tightly packed (a row is extent.x texels). The image must be in TransferSrc
        /// layout. Used by the entity-id picking readback to stage just the search window under the
        /// cursor rather than the whole target.
        /// @param image   Transfer source; only mip 0, array layer 0 is copied.
        /// @param buffer  Transfer destination; receives extent.x*extent.y texels from byte 0.
        /// @param offset  Top-left texel of the region in the image.
        /// @param extent  Region size in texels.
        void CopyImageRegionToBuffer(const Ref<Image>& image, const Ref<Buffer>& buffer,
                                     uvec2 offset, uvec2 extent);

        /// @brief Copies one mip level of one array layer of an image into a buffer at byte 0.
        ///
        /// The whole level is copied, tightly packed, with the depth axis folded in so a Type3D
        /// image's level spans all its z slices. CopyImageToBuffer is the mip-0, layer-0 case; this
        /// is what a layered or mipped image's readback needs.
        /// @param image      Transfer source; must be in TransferSrc layout.
        /// @param buffer     Transfer destination; receives the level from byte 0.
        /// @param mipLevel   The mip level to copy.
        /// @param arrayLayer The array layer to copy.
        void CopyImageSubresourceToBuffer(const Ref<Image>& image, const Ref<Buffer>& buffer,
                                          u32 mipLevel, u32 arrayLayer);

        /// @brief Copies one image's mip 0 into another's, whole-extent.
        ///
        /// Records a single image copy over the full extent of mip 0, array layer 0 — the
        /// depth axis included, so a Type3D image copies all its z slices. The two images must
        /// share extent and a copy-compatible format. Like the other copy recordings this sets no
        /// barriers: @p source must already be in TransferSrc layout and @p destination in
        /// TransferDst — within a RenderGraph those fall out of a TransferSrc/TransferDst
        /// declaration, out of a graph they are the caller's PrepareForAccess.
        /// @param source       Transfer source; must be in TransferSrc layout.
        /// @param destination  Transfer destination; must be in TransferDst layout, same extent.
        void CopyImage(const Ref<Image>& source, const Ref<Image>& destination);

        /// @brief Copies runs of array layers' mip 0 from one image into another, whole-extent.
        ///
        /// One image copy with a region per run, so a pool reallocated at a new layer count
        /// moves its live layers — compacted or not — in one recording. The images share extent
        /// and a copy-compatible format; like CopyImage this sets no barriers, so the touched source
        /// layers must be in TransferSrc layout and the destination layers in TransferDst.
        /// @param source       Transfer source.
        /// @param destination  Transfer destination, same extent.
        /// @param copies       The layer runs; an empty span records nothing.
        void CopyImageLayers(const Ref<Image>& source, const Ref<Image>& destination,
                             std::span<const ImageLayerCopy> copies);

        /// @brief Copies regions between two images, each at any mip level, layer run and offset.
        ///
        /// Records one copy command carrying every region. The images share a copy-compatible
        /// format; like CopyImage this sets no barriers, so the touched source subresources must
        /// be in TransferSrc layout and the destination subresources in TransferDst. The source
        /// and destination may be one image when no region's two subresources overlap.
        /// @param source       Transfer source.
        /// @param destination  Transfer destination.
        /// @param regions      The regions; an empty span records nothing.
        void CopyImageRegions(const Ref<Image>& source, const Ref<Image>& destination,
                              std::span<const ImageCopyRegion> regions);

        /// @brief Blits one image region into another, through a linear filter.
        ///
        /// Like the copy recordings this sets no barriers: the source subresources must be in
        /// TransferSrc layout and the destination subresources in TransferDst. The source and
        /// destination may be one image when the two mip levels differ, which is how a 2x2 box
        /// reduction down a mip chain is recorded.
        /// @param info The images, levels, layers and regions.
        void BlitImage(const BlitImageInfo& info);

        /// @brief Clears every mip level and array layer of a colour image to one value.
        ///
        /// Sets no barriers: the image must be in TransferDst layout.
        /// @param image The image; a colour, non-compressed format.
        /// @param value The clear colour, in the image format's channel order.
        void ClearColorImage(const Ref<Image>& image, vec4 value);

        /// @brief Fills a whole buffer with a repeated 32-bit value.
        ///
        /// Sets no barriers; the fill is a transfer write (PrepareForAccess with
        /// AccessKind::TransferDst names it).
        /// @param buffer The buffer, created with BufferUsage::TransferDst.
        /// @param value  The value written to every 4-byte word.
        void FillBuffer(const Ref<Buffer>& buffer, u32 value);

        /// @brief Transitions a view's image so an out-of-graph consumer can use it as `kind`.
        ///
        /// Within a RenderGraph, barriers fall out of declared use and this is never needed.
        /// This is the deliberate escape hatch for reads/writes the graph cannot see (e.g.
        /// AccessKind::SampleGraphics before ImGui samples a scene texture via ImGui::Image). It
        /// funnels into the same barrier path as the graph (ScopeFor + DecideBarrier) and
        /// updates the image's tracked state, so a later graph pass declaring the same use
        /// correctly sees no hazard.
        /// @param view The view whose underlying image to transition.
        /// @param kind The access kind the image should be prepared for.
        void PrepareForAccess(const Ref<ImageView>& view, AccessKind kind);

        /// @brief Orders an out-of-graph use of a buffer after an earlier one.
        ///
        /// The buffer counterpart of the image overload, for work recorded outside a RenderGraph
        /// between two dispatches or draws that touch one buffer. A buffer carries no tracked
        /// state, so both sides are named: @p previous is every access the earlier work made,
        /// @p next every access the later work will make, each folded into one scope. A barrier is
        /// recorded whatever the two sides are.
        /// @param buffer   The buffer both uses touch.
        /// @param previous The buffer access kinds of the earlier use.
        /// @param next     The buffer access kinds of the later use.
        /// @pre Every kind is a buffer access kind (IsBufferAccess), or TransferSrc / TransferDst
        ///      for a copy or fill touching the buffer.
        void PrepareForAccess(const Ref<Buffer>& buffer, std::initializer_list<AccessKind> previous,
                              std::initializer_list<AccessKind> next);

    private:
        CommandBuffer(Context& context, CommandBufferLevel level);

        /// @brief The owning context.
        Context& m_Context;
        /// @brief Primary or secondary command buffer level.
        CommandBufferLevel m_Level;
        /// @brief Backend command buffer.
        Unique<Native> m_Native;
        /// @brief Layout of the most recently bound pipeline; used for typed push-constant lookup.
        Ref<PipelineLayout> m_LastBoundPipelineLayout;

        /// @brief Checks the bound pipeline's attachment formats against the active rendering's.
        ///
        /// Debug builds only, and once per pipeline bind within a rendering scope rather than per
        /// draw: neither side changes between two draws, so the first draw after either changes is
        /// the one place a mismatch can appear.
        void ValidateAttachmentFormats();

        /// @brief Attachment-format validation state captured from BeginRendering and BindPipeline.
        ///
        /// Compared at draw time to turn a silent dynamic-rendering validation error into a
        /// named VE_ASSERT. Cleared in EndRendering.
        vector<Format> m_ActiveColorAttachmentFormats;
        /// @brief Active depth format.
        Format m_ActiveDepthAttachmentFormat = Format::Undefined;
        /// @brief True while inside BeginRendering/EndRendering.
        bool m_HasActiveRenderingInfo = false;

        /// @brief The bound graphics pipeline, whose declared formats the draw-time check reads;
        ///        null while none (or a compute pipeline) is bound.
        Ref<GraphicsPipeline> m_BoundGraphicsPipeline;
        /// @brief True once the current pipeline and rendering scope have been checked.
        bool m_AttachmentFormatsValidated = false;
        /// @brief Draw commands recorded since the last Begin.
        u32 m_DrawCallCount = 0;
    };

    template <typename T>
    void CommandBuffer::PushConstants(const T& value, const u32 offset)
    {
        static_assert(sizeof(T) <= 128,
                      "Push constant value exceeds the guaranteed minimum block size (128 bytes)");

        VE_ASSERT(m_LastBoundPipelineLayout, "PushConstants<T>: no pipeline layout is bound");

        const auto& ranges = m_LastBoundPipelineLayout->GetPushConstantRanges();

        const PushConstantRange* matchingRange = nullptr;
        for (const auto& range : ranges)
        {
            if (offset >= range.Offset && offset + sizeof(T) <= range.Offset + range.Size)
            {
                VE_ASSERT(matchingRange == nullptr,
                          "PushConstants<T>: [{}, {}) is contained by more than one declared "
                          "PushConstantRange on '{}'",
                          offset, offset + sizeof(T), m_LastBoundPipelineLayout->GetName());

                matchingRange = &range;
            }
        }

        VE_ASSERT(matchingRange != nullptr,
                  "PushConstants<T>: no declared PushConstantRange on '{}' contains [{}, {})",
                  m_LastBoundPipelineLayout->GetName(), offset, offset + sizeof(T));

        PushConstants(PushConstantsInfo{
            .StageFlags = matchingRange->Stages,
            .Offset = offset,
            .Size = sizeof(T),
            .Data = &value,
        });
    }
}
