#pragma once

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DynamicGraphicsPipeline.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Types.h>


namespace Veng::Renderer
{
    template <typename V> class VertexBuffer;
    class IndexBuffer;

    struct PushConstantsInfo
    {
        PipelineLayout& PipelineLayout;
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
    };

    struct BlitImageInfo
    {
        Image& SourceImage;
        Image& DestinationImage;
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
        static Ref<CommandBuffer> Create(CommandBufferLevel level = CommandBufferLevel::Primary)
        {
            return CreateRef<CommandBuffer>(level);
        }

        ~CommandBuffer();
        void BindVertexBuffer(const Ref<Buffer>& buffer);
        void BindIndexBuffer(const Ref<Buffer>& buffer, IndexType type = IndexType::U32);

        // Typed binds (definitions in TypedBuffers.h): no usage flags or index
        // width restated at the call site.
        template <typename V>
        void BindVertexBuffer(const VertexBuffer<V>& buffer);
        void BindIndexBuffer(const IndexBuffer& buffer);

        explicit CommandBuffer(CommandBufferLevel level);

        struct Native;
        [[nodiscard]] Native& GetNative() const;

        void Reset();

        void Begin(CommandBufferUsage flags = CommandBufferUsage::None) const;
        void End() const;

        void BeginRendering(const RenderingInfo& info);
        void EndRendering() const;

        void PushConstants(const PushConstantsInfo& info) const;

        void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) const;
        void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset, u32 firstInstance) const;

        void BindPipeline(const Ref<DynamicGraphicsPipeline>& pipeline);
        void BindPipeline(const Ref<ComputePipeline>& pipeline);

        void SetScissor(ivec2 offset, uvec2 extent) const;
        void SetViewport(ivec2 offset, uvec2 extent) const;

        void BindDescriptorSets(const DescriptorSetBindInfo& info);
        void DrawFullscreenTriangle() const;

        void CopyBufferToImage(const Buffer& buffer, const Image& image) const;
        void CopyImageToBuffer(const Image& image, const Buffer& buffer) const;
        void BlitImage(const BlitImageInfo& info) const;

    private:
        CommandBufferLevel m_Level;
        Unique<Native> m_Native;
        Ref<PipelineLayout> m_LastBoundPipelineLayout{};
    };
}
