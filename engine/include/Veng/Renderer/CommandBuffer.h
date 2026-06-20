#pragma once

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
        /// @brief The view to attach.
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
        vector<RenderingAttachmentInfo> ColorAttachments{};
        /// @brief Optional depth attachment.
        optional<RenderingAttachmentInfo> DepthAttachment{};
        /// @brief Optional stencil attachment.
        optional<RenderingAttachmentInfo> StencilAttachment{};
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
    };

    /// @brief Records Vulkan commands on behalf of the render graph and direct callers.
    class CommandBuffer
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

        /// @brief Records a compute dispatch.
        void Dispatch(u32 groupsX, u32 groupsY, u32 groupsZ);

        /// @brief Binds a graphics pipeline.
        void BindPipeline(const Ref<GraphicsPipeline>& pipeline);

        /// @brief Binds a compute pipeline.
        void BindPipeline(const Ref<ComputePipeline>& pipeline);

        /// @brief Sets the scissor rectangle.
        void SetScissor(ivec2 offset, uvec2 extent);

        /// @brief Sets the viewport.
        void SetViewport(ivec2 offset, uvec2 extent);

        /// @brief Binds descriptor sets.
        void BindDescriptorSets(const DescriptorSetBindInfo& info);

        /// @brief Records a fullscreen triangle draw (no vertex buffer needed).
        void DrawFullscreenTriangle();

        /// @brief Copies the contents of a buffer into an image.
        void CopyBufferToImage(const Ref<Buffer>& buffer, const Ref<Image>& image);

        /// @brief Copies the contents of an image into a buffer.
        void CopyImageToBuffer(const Ref<Image>& image, const Ref<Buffer>& buffer);

        /// @brief Blits one image region into another.
        void BlitImage(const BlitImageInfo& info);

        /// @brief Transitions a view's image so an out-of-graph consumer can use it as `kind`.
        ///
        /// Within a RenderGraph, barriers fall out of declared use and this is never needed.
        /// This is the deliberate escape hatch for reads/writes the graph cannot see (e.g.
        /// AccessKind::Sample before ImGui samples a scene texture via ImGui::Image). It
        /// funnels into the same barrier path as the graph (ScopeFor + DecideBarrier) and
        /// updates the image's tracked state, so a later graph pass declaring the same use
        /// correctly sees no hazard.
        /// @param view The view whose underlying image to transition.
        /// @param kind The access kind the image should be prepared for.
        void PrepareForAccess(const Ref<ImageView>& view, AccessKind kind);

    private:
        CommandBuffer(Context& context, CommandBufferLevel level);

        /// @brief The owning context.
        Context& m_Context;
        /// @brief Primary or secondary command buffer level.
        CommandBufferLevel m_Level;
        /// @brief Backend Vulkan command buffer.
        Unique<Native> m_Native;
        /// @brief Layout of the most recently bound pipeline; used for typed push-constant lookup.
        Ref<PipelineLayout> m_LastBoundPipelineLayout{};

        /// @brief Attachment-format validation state captured from BeginRendering and BindPipeline.
        ///
        /// Compared at draw time to turn a silent dynamic-rendering validation error into a
        /// named VE_ASSERT. Cleared in EndRendering.
        vector<Format> m_ActiveColorAttachmentFormats{};
        /// @brief Active depth format.
        Format m_ActiveDepthAttachmentFormat = Format::Undefined;
        /// @brief True while inside BeginRendering/EndRendering.
        bool m_HasActiveRenderingInfo = false;

        /// @brief Color attachment formats declared by the bound pipeline.
        vector<Format> m_BoundPipelineColorAttachmentFormats{};
        /// @brief Depth format declared by the bound pipeline.
        Format m_BoundPipelineDepthAttachmentFormat = Format::Undefined;
        /// @brief True once a graphics pipeline has been bound.
        bool m_HasBoundGraphicsPipelineFormats = false;
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
