#include <Veng/Renderer/CommandBuffer.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Renderer/Backend/Utils.h>
#include <Veng/Renderer/Native.h>


namespace Veng::Renderer
{
    CommandBuffer::Native& CommandBuffer::GetNative() const { return *m_Native; }

    CommandBuffer::CommandBuffer(const CommandBufferLevel level) :
        m_Level(level), m_Native(CreateUnique<Native>())
    {
        const vk::CommandBufferAllocateInfo allocInfo{
            .commandPool = Context::Instance().GetNative().CommandPool->GetVkCommandPool(),
            .level = ToVk(m_Level),
            .commandBufferCount = 1,
        };

        m_Native->CommandBuffer = GetVkDevice(Context::Instance()).allocateCommandBuffers(allocInfo).value[0];
    }

    void CommandBuffer::Reset()
    {
        m_Native->CommandBuffer.reset();
    }

    void CommandBuffer::Begin(CommandBufferUsage flags) const
    {
        m_Native->CommandBuffer.begin({
            .flags = ToVk(flags)
        });
    }

    static vk::RenderingAttachmentInfo BeginRendering_CreateAttachment(const RenderingAttachmentInfo& info)
    {
        return {
            .imageView = info.ImageView->GetNative().ImageView,
            .imageLayout = Utils::GetFormatAttachmentImageLayout(ToVk(info.ImageView->GetFormat())),
            .resolveMode = vk::ResolveModeFlagBits::eNone,
            .loadOp = ToVk(info.LoadOp),
            .storeOp = ToVk(info.StoreOp),
            .clearValue = ToVk(info.ClearValue)
        };
    }

    void CommandBuffer::BeginRendering(const RenderingInfo& info)
    {
        vector<vk::RenderingAttachmentInfo> colorAttachments;
        colorAttachments.reserve(info.ColorAttachments.size());

        for (auto attachment : info.ColorAttachments)
        {
            colorAttachments.push_back(BeginRendering_CreateAttachment(attachment));
        }

        vk::RenderingAttachmentInfo depthAttachment;
        bool hasDepthAttachment = false;
        if (info.DepthAttachment.has_value())
        {
            depthAttachment = BeginRendering_CreateAttachment(info.DepthAttachment.value());
            hasDepthAttachment = true;
        }

        vk::RenderingAttachmentInfo stencilAttachment;
        bool hasStencilAttachment = false;
        if (info.StencilAttachment.has_value())
        {
            stencilAttachment = BeginRendering_CreateAttachment(info.StencilAttachment.value());
            hasStencilAttachment = true;
        }

        const auto renderingInfo = vk::RenderingInfo{
            .renderArea = {info.Offset.x, info.Offset.y, info.Extent.x, info.Extent.y},
            .layerCount = info.LayerCount,
            .viewMask = info.ViewMask,
            .colorAttachmentCount = static_cast<u32>(colorAttachments.size()),
            .pColorAttachments = colorAttachments.data(),
            .pDepthAttachment = hasDepthAttachment ? &depthAttachment : nullptr,
            .pStencilAttachment = hasStencilAttachment ? &stencilAttachment : nullptr,
        };

        m_Native->CommandBuffer.beginRendering(renderingInfo);
    }

    void CommandBuffer::EndRendering() const
    {
        m_Native->CommandBuffer.endRendering();
    }

    void CommandBuffer::PushConstants(const PushConstantsInfo& info) const
    {
        m_Native->CommandBuffer.pushConstants(info.PipelineLayout.GetNative().Layout, ToVk(info.StageFlags), info.Offset,
                                        info.Size, info.Data);
    }

    void CommandBuffer::BindDescriptorSets(const DescriptorSetBindInfo& info)
    {
        vector<vk::DescriptorSet> descriptorSets;
        descriptorSets.reserve(info.Sets.size());

        for (auto& descriptorSet : info.Sets)
        {
            descriptorSets.push_back(descriptorSet->GetNative().Set);
        }

        m_Native->CommandBuffer.bindDescriptorSets(ToVk(info.PipelineBindPoint), m_LastBoundPipelineLayout->GetNative().Layout,
                                             info.FirstSet, descriptorSets, nullptr);
    }

    void CommandBuffer::DrawFullscreenTriangle() const
    {
        m_Native->CommandBuffer.draw(3, 1, 0, 0);
    }

    void CommandBuffer::Draw(const u32 vertexCount, const u32 instanceCount, const u32 firstVertex,
                             const u32 firstInstance) const
    {
        m_Native->CommandBuffer.draw(vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void CommandBuffer::DrawIndexed(const u32 indexCount, const u32 instanceCount, const u32 firstIndex,
                                    const i32 vertexOffset, const u32 firstInstance) const
    {
        m_Native->CommandBuffer.drawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void CommandBuffer::BindPipeline(const Ref<DynamicGraphicsPipeline>& pipeline)
    {
        m_Native->CommandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->GetNative().Pipeline);
        m_LastBoundPipelineLayout = pipeline->GetPipelineLayout();
    }

    void CommandBuffer::BindPipeline(const Ref<ComputePipeline>& pipeline)
    {
        m_Native->CommandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->GetNative().Pipeline);
        m_LastBoundPipelineLayout = pipeline->GetPipelineLayout();
    }

    void CommandBuffer::SetScissor(const ivec2 offset, const uvec2 extent) const
    {
        m_Native->CommandBuffer.setScissor(0, {vk::Rect2D{{offset.x, offset.y}, {extent.x, extent.y}}});
    }

    void CommandBuffer::SetViewport(const ivec2 offset, const uvec2 extent) const
    {
        m_Native->CommandBuffer.setViewport(0, {
                                          vk::Viewport{
                                              static_cast<f32>(offset.x), static_cast<f32>(offset.y),
                                              static_cast<f32>(extent.x), static_cast<f32>(extent.y), 0.0f, 1.0f
                                          }
                                      });
    }


    void CommandBuffer::End() const
    {
        m_Native->CommandBuffer.end();
    }

    CommandBuffer::~CommandBuffer()
    {
        GetVkDevice(Context::Instance()).freeCommandBuffers(
            Context::Instance().GetNative().CommandPool->GetVkCommandPool(),
            1, &m_Native->CommandBuffer
        );
    }

    void CommandBuffer::BindVertexBuffer(const Ref<Buffer>& buffer)
    {
        m_Native->CommandBuffer.bindVertexBuffers(0, buffer->GetNative().Buffer, {0});
    }

    void CommandBuffer::BindIndexBuffer(const Ref<Buffer>& buffer, const IndexType type)
    {
        m_Native->CommandBuffer.bindIndexBuffer(buffer->GetNative().Buffer, 0, ToVk(type));
    }

    void CommandBuffer::CopyBufferToImage(const Buffer& buffer, const Image& image) const
    {
        const auto extent = image.GetExtent();

        const vk::BufferImageCopy region = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = Utils::GetAspectFlags(ToVk(image.GetFormat())),
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.GetLayers()
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent.x, extent.y, extent.z}
        };

        m_Native->CommandBuffer.copyBufferToImage(
            buffer.GetNative().Buffer,
            image.GetNative().Image,
            vk::ImageLayout::eTransferDstOptimal,
            1,
            &region);
    }

    void CommandBuffer::CopyImageToBuffer(const Image& image, const Buffer& buffer) const
    {
        const auto extent = image.GetExtent();

        const vk::BufferImageCopy region = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = Utils::GetAspectFlags(ToVk(image.GetFormat())),
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent.x, extent.y, extent.z}
        };

        m_Native->CommandBuffer.copyImageToBuffer(image.GetNative().Image, vk::ImageLayout::eTransferSrcOptimal,
                                            buffer.GetNative().Buffer, 1, &region);
    }

    void CommandBuffer::BlitImage(const BlitImageInfo& info) const
    {
        vk::Offset3D sourceOffset = {info.SourceOffset.x, info.SourceOffset.y, info.SourceOffset.z};
        vk::Offset3D sourceExtent = {info.SourceExtent.x, info.SourceExtent.y, info.SourceExtent.z};

        vk::Offset3D destinationOffset = {info.DestinationOffset.x, info.DestinationOffset.y, info.DestinationOffset.z};
        vk::Offset3D destinationExtent = {info.DestinationExtent.x, info.DestinationExtent.y, info.DestinationExtent.z};

        vk::ImageBlit blitInfo{
            .srcSubresource = {
                .aspectMask = Utils::GetAspectFlags(ToVk(info.SourceImage.GetFormat())),
                .mipLevel = info.SourceMipLevel,
                .baseArrayLayer = 0,
                .layerCount = 1
            },
            .srcOffsets = {{sourceOffset, sourceExtent}},
            .dstSubresource = {
                .aspectMask = Utils::GetAspectFlags(ToVk(info.DestinationImage.GetFormat())),
                .mipLevel = info.DestinationMipLevel,
                .baseArrayLayer = 0,
                .layerCount = 1
            },
            .dstOffsets = {{destinationOffset, destinationExtent}}
        };

        m_Native->CommandBuffer.blitImage(
            info.SourceImage.GetNative().Image,
            vk::ImageLayout::eTransferSrcOptimal,
            info.DestinationImage.GetNative().Image,
            vk::ImageLayout::eTransferDstOptimal,
            1,
            &blitInfo,
            vk::Filter::eLinear
        );
    }
}
