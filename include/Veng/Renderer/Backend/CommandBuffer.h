#pragma once

#include <Veng/Renderer/Backend/Buffer.h>
#include <Veng/Renderer/Backend/DescriptorSet.h>
#include <Veng/Renderer/Backend/DynamicGraphicsPipeline.h>
#include <Veng/Renderer/Backend/Framebuffer.h>
#include <Veng/Renderer/Backend/GraphicsPipeline.h>
#include <Veng/Renderer/Backend/ComputePipeline.h>
#include <Veng/Renderer/Backend/ImageBarrier.h>
#include <Veng/Renderer/Backend/RenderPass.h>
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Backend/ImageView.h>


namespace Veng::Renderer
{
    struct PushConstantsInfo
    {
        PipelineLayout& PipelineLayout;
        vk::ShaderStageFlags StageFlags;
        u32 Offset;
        u32 Size;
        const void* Data;
    };

    struct RenderingAttachmentInfo
    {
        Ref<ImageView> ImageView;
        vk::AttachmentLoadOp LoadOp;
        vk::AttachmentStoreOp StoreOp;
        vk::ClearValue ClearValue;
    };

    struct RenderingInfo
    {
        vk::RenderingFlags Flags{};
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
        vk::PipelineBindPoint PipelineBindPoint = vk::PipelineBindPoint::eGraphics;
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
        static Ref<CommandBuffer> Create(vk::CommandBufferLevel level = {})
        {
            return CreateRef<CommandBuffer>(level);
        }

        ~CommandBuffer();
        void BindVertexBuffer(const Ref<Buffer>& buffer);
        void BindIndexBuffer(const Ref<Buffer>& buffer);

        explicit CommandBuffer(vk::CommandBufferLevel level);

        [[nodiscard]] vk::CommandBuffer GetVkCommandBuffer() const { return m_VkCommandBuffer; }

        void Reset();

        void Begin(vk::CommandBufferUsageFlags flags = {}) const;
        void End() const;

        void BeginRenderPass(const Ref<RenderPass>& renderPass, const Ref<Framebuffer>& framebuffer, const vector<vk::ClearValue>& clearValues = {});
        void EndRenderPass() const;

        void BeginRendering(const RenderingInfo& info);
        void EndRendering() const;

        void PushConstants(const PushConstantsInfo& info) const;

        void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) const;
        void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset, u32 firstInstance) const;

        void BindPipeline(const Ref<DynamicGraphicsPipeline>& pipeline);
        void BindPipeline(const Ref<GraphicsPipeline>& pipeline);
        void BindPipeline(const Ref<ComputePipeline>& pipeline);

        void SetScissor(ivec2 offset, uvec2 extent) const;
        void SetViewport(ivec2 offset, uvec2 extent) const;

        void BindDescriptorSets(const DescriptorSetBindInfo& info);
        void DrawFullscreenTriangle() const;

        void PipelineBarrier(const ImageBarrier& barrier) const;

        void CopyBufferToImage(const Buffer& buffer, const Image& image) const;
        void CopyImageToBuffer(const Image& image, const Buffer& buffer) const;
        void BlitImage(const BlitImageInfo& info) const;

    private:
        vk::CommandBufferLevel m_Level;
        vk::CommandBuffer m_VkCommandBuffer;
        vector<Ref<void>> m_BoundResources{};
        Ref<PipelineLayout> m_LastBoundPipelineLayout{};
    };
}
