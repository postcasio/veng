#include <Veng/Renderer/CommandBuffer.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Renderer/Backend/Utils.h>
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Native.h>


namespace Veng::Renderer
{
    CommandBuffer::Native& CommandBuffer::GetNative() const { return *m_Native; }

    CommandBuffer::CommandBuffer(Context& context, const CommandBufferLevel level) :
        m_Context(context), m_Level(level), m_Native(CreateUnique<Native>())
    {
        const vk::CommandBufferAllocateInfo allocInfo{
            .commandPool = m_Context.GetNative().CommandPool->GetVkCommandPool(),
            .level = ToVk(m_Level),
            .commandBufferCount = 1,
        };

        m_Native->CommandBuffer = GetVkDevice(m_Context).allocateCommandBuffers(allocInfo).value[0];
    }

    void CommandBuffer::Reset()
    {
        VK_ASSERT(m_Native->CommandBuffer.reset(), "failed to reset command buffer!");
    }

    void CommandBuffer::Begin(CommandBufferUsage flags)
    {
        VK_ASSERT(m_Native->CommandBuffer.begin({
            .flags = ToVk(flags)
        }), "failed to begin command buffer!");
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

    // Draw-time pipeline/attachment format validation: a dynamic-rendering
    // pipeline declares its attachment formats at creation, and they must match
    // the formats of the views the render graph is currently rendering into —
    // otherwise Vulkan validation raises a silent (easy to miss) error. Checked
    // only when both the active rendering attachments and a bound graphics
    // pipeline's formats are known, so manual/non-graph draws without this info
    // never false-trip.
    static void ValidateBoundPipelineAttachmentFormats(
        const vector<Format>& activeColorFormats, const Format activeDepthFormat,
        const vector<Format>& pipelineColorFormats, const Format pipelineDepthFormat)
    {
        VE_ASSERT(pipelineColorFormats.size() == activeColorFormats.size(),
                  "CommandBuffer::Draw: bound pipeline declares {} color attachment(s) but {} are active",
                  pipelineColorFormats.size(), activeColorFormats.size());

        for (usize i = 0; i < pipelineColorFormats.size(); i++)
        {
            VE_ASSERT(pipelineColorFormats[i] == activeColorFormats[i],
                      "CommandBuffer::Draw: color attachment {} format mismatch — pipeline declares {}, "
                      "render target is {}",
                      i, static_cast<u32>(pipelineColorFormats[i]), static_cast<u32>(activeColorFormats[i]));
        }

        VE_ASSERT(pipelineDepthFormat == activeDepthFormat,
                  "CommandBuffer::Draw: depth attachment format mismatch — pipeline declares {}, render target is {}",
                  static_cast<u32>(pipelineDepthFormat), static_cast<u32>(activeDepthFormat));
    }

    void CommandBuffer::BeginRendering(const RenderingInfo& info)
    {
        // Capture the active attachment formats for draw-time pipeline validation
        // (see Draw/DrawIndexed/DrawFullscreenTriangle).
        m_ActiveColorAttachmentFormats.clear();
        m_ActiveColorAttachmentFormats.reserve(info.ColorAttachments.size());

        for (const auto& attachment : info.ColorAttachments)
        {
            m_ActiveColorAttachmentFormats.push_back(attachment.ImageView->GetFormat());
        }

        m_ActiveDepthAttachmentFormat = info.DepthAttachment.has_value()
            ? info.DepthAttachment->ImageView->GetFormat()
            : Format::Undefined;

        m_HasActiveRenderingInfo = true;

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

    void CommandBuffer::EndRendering()
    {
        m_Native->CommandBuffer.endRendering();

        m_ActiveColorAttachmentFormats.clear();
        m_ActiveDepthAttachmentFormat = Format::Undefined;
        m_HasActiveRenderingInfo = false;
    }

    void CommandBuffer::PushConstants(const PushConstantsInfo& info)
    {
        m_Native->CommandBuffer.pushConstants(m_LastBoundPipelineLayout->GetNative().Layout, ToVk(info.StageFlags), info.Offset,
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

    void CommandBuffer::DrawFullscreenTriangle()
    {
        if (m_HasActiveRenderingInfo && m_HasBoundGraphicsPipelineFormats)
        {
            ValidateBoundPipelineAttachmentFormats(m_ActiveColorAttachmentFormats, m_ActiveDepthAttachmentFormat,
                                                    m_BoundPipelineColorAttachmentFormats,
                                                    m_BoundPipelineDepthAttachmentFormat);
        }

        m_Native->CommandBuffer.draw(3, 1, 0, 0);
    }

    void CommandBuffer::Draw(const u32 vertexCount, const u32 instanceCount, const u32 firstVertex,
                             const u32 firstInstance)
    {
        if (m_HasActiveRenderingInfo && m_HasBoundGraphicsPipelineFormats)
        {
            ValidateBoundPipelineAttachmentFormats(m_ActiveColorAttachmentFormats, m_ActiveDepthAttachmentFormat,
                                                    m_BoundPipelineColorAttachmentFormats,
                                                    m_BoundPipelineDepthAttachmentFormat);
        }

        m_Native->CommandBuffer.draw(vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void CommandBuffer::DrawIndexed(const u32 indexCount, const u32 instanceCount, const u32 firstIndex,
                                    const i32 vertexOffset, const u32 firstInstance)
    {
        if (m_HasActiveRenderingInfo && m_HasBoundGraphicsPipelineFormats)
        {
            ValidateBoundPipelineAttachmentFormats(m_ActiveColorAttachmentFormats, m_ActiveDepthAttachmentFormat,
                                                    m_BoundPipelineColorAttachmentFormats,
                                                    m_BoundPipelineDepthAttachmentFormat);
        }

        m_Native->CommandBuffer.drawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void CommandBuffer::Dispatch(const u32 groupsX, const u32 groupsY, const u32 groupsZ)
    {
        m_Native->CommandBuffer.dispatch(groupsX, groupsY, groupsZ);
    }

    void CommandBuffer::BindPipeline(const Ref<GraphicsPipeline>& pipeline)
    {
        m_Native->CommandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->GetNative().Pipeline);
        m_LastBoundPipelineLayout = pipeline->GetPipelineLayout();

        // Capture the declared attachment formats for draw-time validation
        // against the active rendering attachments (see Draw/DrawIndexed/
        // DrawFullscreenTriangle).
        m_BoundPipelineColorAttachmentFormats = pipeline->GetColorAttachmentFormats();
        m_BoundPipelineDepthAttachmentFormat = pipeline->GetDepthAttachmentFormat();
        m_HasBoundGraphicsPipelineFormats = true;
    }

    void CommandBuffer::BindPipeline(const Ref<ComputePipeline>& pipeline)
    {
        m_Native->CommandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->GetNative().Pipeline);
        m_LastBoundPipelineLayout = pipeline->GetPipelineLayout();

        // A compute pipeline has no rendering attachments; clear any previously
        // bound graphics pipeline's formats so draw-time validation does not
        // compare against a stale graphics pipeline.
        m_BoundPipelineColorAttachmentFormats.clear();
        m_BoundPipelineDepthAttachmentFormat = Format::Undefined;
        m_HasBoundGraphicsPipelineFormats = false;
    }

    void CommandBuffer::SetScissor(const ivec2 offset, const uvec2 extent)
    {
        m_Native->CommandBuffer.setScissor(0, {vk::Rect2D{{offset.x, offset.y}, {extent.x, extent.y}}});
    }

    void CommandBuffer::SetViewport(const ivec2 offset, const uvec2 extent)
    {
        m_Native->CommandBuffer.setViewport(0, {
                                          vk::Viewport{
                                              static_cast<f32>(offset.x), static_cast<f32>(offset.y),
                                              static_cast<f32>(extent.x), static_cast<f32>(extent.y), 0.0f, 1.0f
                                          }
                                      });
    }


    void CommandBuffer::End()
    {
        VK_ASSERT(m_Native->CommandBuffer.end(), "failed to end command buffer!");
    }

    CommandBuffer::~CommandBuffer()
    {
        GetVkDevice(m_Context).freeCommandBuffers(
            m_Context.GetNative().CommandPool->GetVkCommandPool(),
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

    void CommandBuffer::CopyBufferToImage(const Ref<Buffer>& buffer, const Ref<Image>& image)
    {
        const auto extent = image->GetExtent();

        const vk::BufferImageCopy region = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = Utils::GetAspectFlags(ToVk(image->GetFormat())),
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image->GetLayers()
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent.x, extent.y, extent.z}
        };

        m_Native->CommandBuffer.copyBufferToImage(
            buffer->GetNative().Buffer,
            image->GetNative().Image,
            vk::ImageLayout::eTransferDstOptimal,
            1,
            &region);
    }

    void CommandBuffer::CopyImageToBuffer(const Ref<Image>& image, const Ref<Buffer>& buffer)
    {
        const auto extent = image->GetExtent();

        const vk::BufferImageCopy region = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = Utils::GetAspectFlags(ToVk(image->GetFormat())),
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent.x, extent.y, extent.z}
        };

        m_Native->CommandBuffer.copyImageToBuffer(image->GetNative().Image, vk::ImageLayout::eTransferSrcOptimal,
                                            buffer->GetNative().Buffer, 1, &region);
    }

    void CommandBuffer::BlitImage(const BlitImageInfo& info)
    {
        vk::Offset3D sourceOffset = {info.SourceOffset.x, info.SourceOffset.y, info.SourceOffset.z};
        vk::Offset3D sourceExtent = {info.SourceExtent.x, info.SourceExtent.y, info.SourceExtent.z};

        vk::Offset3D destinationOffset = {info.DestinationOffset.x, info.DestinationOffset.y, info.DestinationOffset.z};
        vk::Offset3D destinationExtent = {info.DestinationExtent.x, info.DestinationExtent.y, info.DestinationExtent.z};

        vk::ImageBlit blitInfo{
            .srcSubresource = {
                .aspectMask = Utils::GetAspectFlags(ToVk(info.SourceImage->GetFormat())),
                .mipLevel = info.SourceMipLevel,
                .baseArrayLayer = 0,
                .layerCount = 1
            },
            .srcOffsets = {{sourceOffset, sourceExtent}},
            .dstSubresource = {
                .aspectMask = Utils::GetAspectFlags(ToVk(info.DestinationImage->GetFormat())),
                .mipLevel = info.DestinationMipLevel,
                .baseArrayLayer = 0,
                .layerCount = 1
            },
            .dstOffsets = {{destinationOffset, destinationExtent}}
        };

        m_Native->CommandBuffer.blitImage(
            info.SourceImage->GetNative().Image,
            vk::ImageLayout::eTransferSrcOptimal,
            info.DestinationImage->GetNative().Image,
            vk::ImageLayout::eTransferDstOptimal,
            1,
            &blitInfo,
            vk::Filter::eLinear
        );
    }
}
