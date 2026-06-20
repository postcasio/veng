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
    template <typename V> class VertexBuffer;
    class IndexBuffer;
    class Context;

    // Raw, untyped push for exotic/dynamic use (e.g. a single push spanning
    // ranges with different stages). The layout comes from the command
    // buffer's last-bound pipeline layout, same as descriptor set binds.
    // Prefer the typed CommandBuffer::PushConstants<T>, which also derives
    // StageFlags and Size from the declared PushConstantRange.
    struct PushConstantsInfo
    {
        ShaderStage StageFlags;
        u32 Offset;
        u32 Size;
        const void* Data;
    };

    struct RenderingAttachmentInfo
    {
        Ref<ImageView> ImageView;
        LoadOp LoadOp;
        StoreOp StoreOp;
        ClearValue ClearValue;
    };

    struct RenderingInfo
    {
        ivec2 Offset = {0, 0};
        uvec2 Extent;
        u32 LayerCount = 1;
        u32 ViewMask = 0;
        vector<RenderingAttachmentInfo> ColorAttachments{};
        optional<RenderingAttachmentInfo> DepthAttachment{};
        optional<RenderingAttachmentInfo> StencilAttachment{};
    };

    struct DescriptorSetBindInfo
    {
        vector<Ref<DescriptorSet>> Sets;
        u32 FirstSet = 0;
        PipelineBindPoint PipelineBindPoint = PipelineBindPoint::Graphics;
        // One byte offset per UniformBufferDynamic / StorageBufferDynamic
        // descriptor across the bound sets, in binding order — selects the live
        // region of a ring-buffered dynamic buffer at bind. Empty for sets with no
        // dynamic descriptors.
        vector<u32> DynamicOffsets;
    };

    struct BlitImageInfo
    {
        Ref<Image> SourceImage;
        Ref<Image> DestinationImage;
        u32 SourceMipLevel;
        u32 DestinationMipLevel;
        ivec3 SourceOffset;
        ivec3 DestinationOffset;
        ivec3 SourceExtent;
        ivec3 DestinationExtent;
    };

    class CommandBuffer
    {
    public:
        static Ref<CommandBuffer> Create(Context& context, CommandBufferLevel level = CommandBufferLevel::Primary)
        {
            return Ref<CommandBuffer>(new CommandBuffer(context, level));
        }

        ~CommandBuffer();

        CommandBuffer(const CommandBuffer&) = delete;
        CommandBuffer& operator=(const CommandBuffer&) = delete;

        void BindVertexBuffer(const Ref<Buffer>& buffer);
        void BindIndexBuffer(const Ref<Buffer>& buffer, IndexType type = IndexType::U32);

        // Typed binds (definitions in TypedBuffers.h): no usage flags or index
        // width restated at the call site.
        template <typename V>
        void BindVertexBuffer(const VertexBuffer<V>& buffer);
        void BindIndexBuffer(const IndexBuffer& buffer);

        struct Native;
        [[nodiscard]] Native& GetNative() const;

        void Reset();

        void Begin(CommandBufferUsage flags = CommandBufferUsage::None);
        void End();

        void BeginRendering(const RenderingInfo& info);
        void EndRendering();

        void PushConstants(const PushConstantsInfo& info);

        // Typed push. Stages and size come from the PushConstantRange declared
        // on the bound pipeline layout that contains [offset, offset +
        // sizeof(T)); only the value (and an optional offset for partial or
        // multi-range pushes) come from the call site.
        template <typename T>
        void PushConstants(const T& value, u32 offset = 0);

        void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance);
        void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset, u32 firstInstance);

        void Dispatch(u32 groupsX, u32 groupsY, u32 groupsZ);

        void BindPipeline(const Ref<GraphicsPipeline>& pipeline);
        void BindPipeline(const Ref<ComputePipeline>& pipeline);

        void SetScissor(ivec2 offset, uvec2 extent);
        void SetViewport(ivec2 offset, uvec2 extent);

        void BindDescriptorSets(const DescriptorSetBindInfo& info);
        void DrawFullscreenTriangle();

        void CopyBufferToImage(const Ref<Buffer>& buffer, const Ref<Image>& image);
        void CopyImageToBuffer(const Ref<Image>& image, const Ref<Buffer>& buffer);
        void BlitImage(const BlitImageInfo& info);

        // Transition a view's image so an out-of-graph consumer can use it as
        // `kind` (e.g. AccessKind::Sample before ImGui samples a scene texture
        // via ImGui::Image). Within a RenderGraph, barriers fall out of declared
        // use and you never call this; this is the deliberate, named escape hatch
        // for reads/writes the graph cannot see. It funnels into the same
        // barrier path as the graph (ScopeFor + DecideBarrier) and updates the
        // image's tracked state, so a later graph pass declaring the same use
        // correctly sees no hazard.
        void PrepareForAccess(const Ref<ImageView>& view, AccessKind kind);

    private:
        CommandBuffer(Context& context, CommandBufferLevel level);

        Context& m_Context;
        CommandBufferLevel m_Level;
        Unique<Native> m_Native;
        Ref<PipelineLayout> m_LastBoundPipelineLayout{};

        // Attachment-format validation (see Draw/DrawIndexed/DrawFullscreenTriangle):
        // captured from the active RenderingInfo in BeginRendering and from the
        // bound pipeline in BindPipeline(GraphicsPipeline). Compared at
        // draw time to turn a silent dynamic-rendering validation error into a
        // named VE_ASSERT. Cleared in EndRendering.
        vector<Format> m_ActiveColorAttachmentFormats{};
        Format m_ActiveDepthAttachmentFormat = Format::Undefined;
        bool m_HasActiveRenderingInfo = false;

        vector<Format> m_BoundPipelineColorAttachmentFormats{};
        Format m_BoundPipelineDepthAttachmentFormat = Format::Undefined;
        bool m_HasBoundGraphicsPipelineFormats = false;
    };

    template <typename T>
    void CommandBuffer::PushConstants(const T& value, const u32 offset)
    {
        static_assert(sizeof(T) <= 128, "Push constant value exceeds the guaranteed minimum block size (128 bytes)");

        VE_ASSERT(m_LastBoundPipelineLayout, "PushConstants<T>: no pipeline layout is bound");

        const auto& ranges = m_LastBoundPipelineLayout->GetPushConstantRanges();

        const PushConstantRange* matchingRange = nullptr;
        for (const auto& range : ranges)
        {
            if (offset >= range.Offset && offset + sizeof(T) <= range.Offset + range.Size)
            {
                VE_ASSERT(matchingRange == nullptr,
                          "PushConstants<T>: [{}, {}) is contained by more than one declared PushConstantRange on '{}'",
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
