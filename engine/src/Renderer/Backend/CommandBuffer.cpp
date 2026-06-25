#include <Veng/Renderer/CommandBuffer.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Backend/Barrier.h>
#include <Veng/Renderer/Backend/BarrierDecision.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Renderer/Backend/Utils.h>
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Native.h>

namespace Veng::Renderer
{
    CommandBuffer::Native& CommandBuffer::GetNative() const
    {
        return *m_Native;
    }

    CommandBuffer::CommandBuffer(Context& context, const CommandBufferLevel level)
        : m_Context(context), m_Level(level), m_Native(CreateUnique<Native>())
    {
        // Allocation normally comes from the shared graphics pool. The transfer
        // setup sets AllocationPoolOverride so a worker's transfer command buffer
        // is allocated from that worker's own (single-thread) transfer pool.
        const vk::CommandPool pool = m_Context.GetNative().AllocationPoolOverride
                                         ? m_Context.GetNative().AllocationPoolOverride
                                         : m_Context.GetNative().CommandPool->GetVkCommandPool();

        const vk::CommandBufferAllocateInfo allocInfo{
            .commandPool = pool,
            .level = ToVk(m_Level),
            .commandBufferCount = 1,
        };

        m_Native->Pool = pool;
        m_Native->CommandBuffer = GetVkDevice(m_Context).allocateCommandBuffers(allocInfo).value[0];
    }

    void CommandBuffer::Reset()
    {
        VK_ASSERT(m_Native->CommandBuffer.reset(), "failed to reset command buffer!");
    }

    void CommandBuffer::Begin(CommandBufferUsage flags)
    {
        VK_ASSERT(m_Native->CommandBuffer.begin({.flags = ToVk(flags)}),
                  "failed to begin command buffer!");
    }

    static vk::RenderingAttachmentInfo
    BeginRendering_CreateAttachment(const RenderingAttachmentInfo& info)
    {
        return {.imageView = info.ImageView->GetNative().ImageView,
                .imageLayout =
                    Utils::GetFormatAttachmentImageLayout(ToVk(info.ImageView->GetFormat())),
                .resolveMode = vk::ResolveModeFlagBits::eNone,
                .loadOp = ToVk(info.LoadOp),
                .storeOp = ToVk(info.StoreOp),
                .clearValue = ToVk(info.ClearValue)};
    }

    // Draw-time pipeline/attachment format validation: a dynamic-rendering
    // pipeline declares its attachment formats at creation, and they must match
    // the formats of the views the render graph is currently rendering into —
    // otherwise Vulkan validation raises a silent (easy to miss) error. Checked
    // only when both the active rendering attachments and a bound graphics
    // pipeline's formats are known, so manual/non-graph draws without this info
    // never false-trip.
    static void ValidateBoundPipelineAttachmentFormats(const vector<Format>& activeColorFormats,
                                                       const Format activeDepthFormat,
                                                       const vector<Format>& pipelineColorFormats,
                                                       const Format pipelineDepthFormat)
    {
        VE_ASSERT(
            pipelineColorFormats.size() == activeColorFormats.size(),
            "CommandBuffer::Draw: bound pipeline declares {} color attachment(s) but {} are active",
            pipelineColorFormats.size(), activeColorFormats.size());

        for (usize i = 0; i < pipelineColorFormats.size(); i++)
        {
            VE_ASSERT(
                pipelineColorFormats[i] == activeColorFormats[i],
                "CommandBuffer::Draw: color attachment {} format mismatch — pipeline declares {}, "
                "render target is {}",
                i, static_cast<u32>(pipelineColorFormats[i]),
                static_cast<u32>(activeColorFormats[i]));
        }

        VE_ASSERT(pipelineDepthFormat == activeDepthFormat,
                  "CommandBuffer::Draw: depth attachment format mismatch — pipeline declares {}, "
                  "render target is {}",
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
            .renderArea = {.offset = {.x = info.Offset.x, .y = info.Offset.y},
                           .extent = {.width = info.Extent.x, .height = info.Extent.y}},
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
        m_Native->CommandBuffer.pushConstants(m_LastBoundPipelineLayout->GetNative().Layout,
                                              ToVk(info.StageFlags), info.Offset, info.Size,
                                              info.Data);
    }

    void CommandBuffer::BindDescriptorSets(const DescriptorSetBindInfo& info)
    {
        vector<vk::DescriptorSet> descriptorSets;
        descriptorSets.reserve(info.Sets.size());

        for (auto& descriptorSet : info.Sets)
        {
            descriptorSets.push_back(descriptorSet->GetNative().Set);
        }

        m_Native->CommandBuffer.bindDescriptorSets(
            ToVk(info.PipelineBindPoint), m_LastBoundPipelineLayout->GetNative().Layout,
            info.FirstSet, descriptorSets, info.DynamicOffsets);
    }

    void CommandBuffer::DrawFullscreenTriangle()
    {
        if (m_HasActiveRenderingInfo && m_HasBoundGraphicsPipelineFormats)
        {
            ValidateBoundPipelineAttachmentFormats(
                m_ActiveColorAttachmentFormats, m_ActiveDepthAttachmentFormat,
                m_BoundPipelineColorAttachmentFormats, m_BoundPipelineDepthAttachmentFormat);
        }

        m_Native->CommandBuffer.draw(3, 1, 0, 0);
    }

    void CommandBuffer::Draw(const u32 vertexCount, const u32 instanceCount, const u32 firstVertex,
                             const u32 firstInstance)
    {
        if (m_HasActiveRenderingInfo && m_HasBoundGraphicsPipelineFormats)
        {
            ValidateBoundPipelineAttachmentFormats(
                m_ActiveColorAttachmentFormats, m_ActiveDepthAttachmentFormat,
                m_BoundPipelineColorAttachmentFormats, m_BoundPipelineDepthAttachmentFormat);
        }

        m_Native->CommandBuffer.draw(vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void CommandBuffer::DrawIndexed(const u32 indexCount, const u32 instanceCount,
                                    const u32 firstIndex, const i32 vertexOffset,
                                    const u32 firstInstance)
    {
        if (m_HasActiveRenderingInfo && m_HasBoundGraphicsPipelineFormats)
        {
            ValidateBoundPipelineAttachmentFormats(
                m_ActiveColorAttachmentFormats, m_ActiveDepthAttachmentFormat,
                m_BoundPipelineColorAttachmentFormats, m_BoundPipelineDepthAttachmentFormat);
        }

        m_Native->CommandBuffer.drawIndexed(indexCount, instanceCount, firstIndex, vertexOffset,
                                            firstInstance);
    }

    void CommandBuffer::DrawIndexedIndirect(const Ref<Buffer>& buffer, const u64 offset,
                                            const u32 drawCount, const u32 stride)
    {
        if (m_HasActiveRenderingInfo && m_HasBoundGraphicsPipelineFormats)
        {
            ValidateBoundPipelineAttachmentFormats(
                m_ActiveColorAttachmentFormats, m_ActiveDepthAttachmentFormat,
                m_BoundPipelineColorAttachmentFormats, m_BoundPipelineDepthAttachmentFormat);
        }

        // vkCmdDrawIndexedIndirect requires a 4-byte-aligned offset.
        VE_ASSERT(offset % 4 == 0, "DrawIndexedIndirect: offset {} is not 4-byte aligned", offset);

        m_Native->CommandBuffer.drawIndexedIndirect(buffer->GetNative().Buffer, offset, drawCount,
                                                    stride);
    }

    void CommandBuffer::Dispatch(const u32 groupsX, const u32 groupsY, const u32 groupsZ)
    {
        m_Native->CommandBuffer.dispatch(groupsX, groupsY, groupsZ);
    }

    void CommandBuffer::BindPipeline(const Ref<GraphicsPipeline>& pipeline)
    {
        m_Native->CommandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                                             pipeline->GetNative().Pipeline);
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
        m_Native->CommandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute,
                                             pipeline->GetNative().Pipeline);
        m_LastBoundPipelineLayout = pipeline->GetPipelineLayout();

        // A compute pipeline has no rendering attachments; clear any previously
        // captured graphics formats so draw-time validation doesn't run against
        // a stale graphics pipeline.
        m_BoundPipelineColorAttachmentFormats.clear();
        m_BoundPipelineDepthAttachmentFormat = Format::Undefined;
        m_HasBoundGraphicsPipelineFormats = false;
    }

    void CommandBuffer::SetScissor(const ivec2 offset, const uvec2 extent)
    {
        m_Native->CommandBuffer.setScissor(
            0, {vk::Rect2D{.offset = {.x = offset.x, .y = offset.y},
                           .extent = {.width = extent.x, .height = extent.y}}});
    }

    void CommandBuffer::SetViewport(const ivec2 offset, const uvec2 extent)
    {
        m_Native->CommandBuffer.setViewport(0, {vk::Viewport{.x = static_cast<f32>(offset.x),
                                                             .y = static_cast<f32>(offset.y),
                                                             .width = static_cast<f32>(extent.x),
                                                             .height = static_cast<f32>(extent.y),
                                                             .minDepth = 0.0f,
                                                             .maxDepth = 1.0f}});
    }

    void CommandBuffer::End()
    {
        VK_ASSERT(m_Native->CommandBuffer.end(), "failed to end command buffer!");
    }

    CommandBuffer::~CommandBuffer()
    {
        GetVkDevice(m_Context).freeCommandBuffers(m_Native->Pool, 1, &m_Native->CommandBuffer);
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
            .imageSubresource = {.aspectMask = Utils::GetAspectFlags(ToVk(image->GetFormat())),
                                 .mipLevel = 0,
                                 .baseArrayLayer = 0,
                                 .layerCount = image->GetLayers()},
            .imageOffset = {.x = 0, .y = 0, .z = 0},
            .imageExtent = {.width = extent.x, .height = extent.y, .depth = extent.z}};

        m_Native->CommandBuffer.copyBufferToImage(buffer->GetNative().Buffer,
                                                  image->GetNative().Image,
                                                  vk::ImageLayout::eTransferDstOptimal, 1, &region);
    }

    void CommandBuffer::CopyBufferToImage(const Ref<Buffer>& buffer, const Ref<Image>& image,
                                          const std::span<const BufferImageCopyRegion> regions)
    {
        const vk::ImageAspectFlags aspect = Utils::GetAspectFlags(ToVk(image->GetFormat()));
        const u32 layerCount = image->GetLayers();

        vector<vk::BufferImageCopy> copies;
        copies.reserve(regions.size());
        for (const BufferImageCopyRegion& region : regions)
        {
            copies.emplace_back(
                vk::BufferImageCopy{.bufferOffset = region.BufferOffset,
                                    .bufferRowLength = 0,
                                    .bufferImageHeight = 0,
                                    .imageSubresource = {.aspectMask = aspect,
                                                         .mipLevel = region.MipLevel,
                                                         .baseArrayLayer = 0,
                                                         .layerCount = layerCount},
                                    .imageOffset = {.x = 0, .y = 0, .z = 0},
                                    .imageExtent = {.width = region.Extent.x,
                                                    .height = region.Extent.y,
                                                    .depth = region.Extent.z}});
        }

        m_Native->CommandBuffer.copyBufferToImage(
            buffer->GetNative().Buffer, image->GetNative().Image,
            vk::ImageLayout::eTransferDstOptimal, static_cast<u32>(copies.size()), copies.data());
    }

    void CommandBuffer::CopyImageToBuffer(const Ref<Image>& image, const Ref<Buffer>& buffer)
    {
        const auto extent = image->GetExtent();

        const vk::BufferImageCopy region = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {.aspectMask = Utils::GetAspectFlags(ToVk(image->GetFormat())),
                                 .mipLevel = 0,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
            .imageOffset = {.x = 0, .y = 0, .z = 0},
            .imageExtent = {.width = extent.x, .height = extent.y, .depth = extent.z}};

        m_Native->CommandBuffer.copyImageToBuffer(image->GetNative().Image,
                                                  vk::ImageLayout::eTransferSrcOptimal,
                                                  buffer->GetNative().Buffer, 1, &region);
    }

    void CommandBuffer::BlitImage(const BlitImageInfo& info)
    {
        vk::Offset3D sourceOffset = {
            .x = info.SourceOffset.x, .y = info.SourceOffset.y, .z = info.SourceOffset.z};
        vk::Offset3D sourceExtent = {
            .x = info.SourceExtent.x, .y = info.SourceExtent.y, .z = info.SourceExtent.z};

        vk::Offset3D destinationOffset = {.x = info.DestinationOffset.x,
                                          .y = info.DestinationOffset.y,
                                          .z = info.DestinationOffset.z};
        vk::Offset3D destinationExtent = {.x = info.DestinationExtent.x,
                                          .y = info.DestinationExtent.y,
                                          .z = info.DestinationExtent.z};

        const vk::ImageBlit blitInfo{
            .srcSubresource = {.aspectMask =
                                   Utils::GetAspectFlags(ToVk(info.SourceImage->GetFormat())),
                               .mipLevel = info.SourceMipLevel,
                               .baseArrayLayer = 0,
                               .layerCount = 1},
            .srcOffsets = {{sourceOffset, sourceExtent}},
            .dstSubresource = {.aspectMask =
                                   Utils::GetAspectFlags(ToVk(info.DestinationImage->GetFormat())),
                               .mipLevel = info.DestinationMipLevel,
                               .baseArrayLayer = 0,
                               .layerCount = 1},
            .dstOffsets = {{destinationOffset, destinationExtent}}};

        m_Native->CommandBuffer.blitImage(
            info.SourceImage->GetNative().Image, vk::ImageLayout::eTransferSrcOptimal,
            info.DestinationImage->GetNative().Image, vk::ImageLayout::eTransferDstOptimal, 1,
            &blitInfo, vk::Filter::eLinear);
    }

    void CommandBuffer::PrepareForAccess(const Ref<ImageView>& view, const AccessKind kind)
    {
        // Same path the RenderGraph uses for a declared access (ScopeFor +
        // TransitionImage), so an out-of-graph use leaves the image's tracked
        // state exactly as a graph pass would — a later graph pass declaring the
        // same use then correctly sees no hazard.
        const auto scope = Backend::ScopeFor(kind);

        Backend::TransitionImage(*this, *view->GetImage(), scope.Layout, scope.Stage, scope.Access,
                                 view->GetBaseArrayLayer(), view->GetArrayLayers(),
                                 view->GetBaseMipLevel(), view->GetMipLevels());
    }
}
