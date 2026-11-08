#include <Veng/Renderer/CommandBuffer.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <utility>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/FormatInfo.h>

#include "Barrier.h"
#include "Blit.h"
#include "Natives.h"
#include "Recording.h"
#include "TypeMapping.h"

namespace Veng::Renderer
{
    static_assert(static_cast<u64>(Metal::MetalStage::Vertex) == MTLStageVertex);
    static_assert(static_cast<u64>(Metal::MetalStage::Fragment) == MTLStageFragment);
    static_assert(static_cast<u64>(Metal::MetalStage::Dispatch) == MTLStageDispatch);
    static_assert(static_cast<u64>(Metal::MetalStage::Blit) == MTLStageBlit);
    static_assert(sizeof(DispatchIndirectCommand) ==
                  sizeof(MTLDispatchThreadgroupsIndirectArguments));

    namespace
    {
        // A copy region's layer count, zero meaning every layer from its base.
        u32 RegionLayers(const Image& image, const BufferImageCopyRegion& region)
        {
            VE_ASSERT(region.BaseArrayLayer < image.GetLayers() &&
                          region.BaseArrayLayer + region.ArrayLayers <= image.GetLayers(),
                      "copy region layers {}+{} exceed image '{}' ({} layers)",
                      region.BaseArrayLayer, region.ArrayLayers, image.GetName(),
                      image.GetLayers());
            return region.ArrayLayers != 0 ? region.ArrayLayers
                                           : image.GetLayers() - region.BaseArrayLayer;
        }

        // The tightly packed pitch of one row and one 2D slice of an extent in a format.
        struct Pitch
        {
            u64 Row = 0;
            u64 Slice = 0;
        };

        Pitch PitchOf(const Format format, const uvec3 extent)
        {
            const FormatBlockInfo block = GetFormatBlockInfo(format);
            VE_ASSERT(block.Bytes > 0, "a copy of {} has no defined texel size",
                      FormatName(format));
            const u64 blocksWide = (extent.x + block.BlockWidth - 1) / block.BlockWidth;
            const u64 blocksHigh = (extent.y + block.BlockHeight - 1) / block.BlockHeight;
            return {.Row = blocksWide * block.Bytes,
                    .Slice = blocksWide * blocksHigh * block.Bytes};
        }

        uvec3 MipExtent(const Image& image, const u32 mip)
        {
            const uvec3 extent = image.GetExtent();
            return {std::max(1u, extent.x >> mip), std::max(1u, extent.y >> mip),
                    image.GetType() == ImageType::Type3D ? std::max(1u, extent.z >> mip) : 1u};
        }

        MTLOrigin ToOrigin(const uvec3 offset)
        {
            return MTLOriginMake(offset.x, offset.y, offset.z);
        }

        MTLSize ToSize(const uvec3 extent)
        {
            return MTLSizeMake(extent.x, extent.y, extent.z);
        }

        // Buffer to texture, one slice at a time (a 3D image's copy is one slice of full depth).
        void CopyToTexture(const CommandBuffer& cmd, const Buffer& buffer, u64 offset,
                           const Image& image, const u32 mip, const u32 baseLayer, const u32 layers,
                           const uvec3 origin, const uvec3 extent)
        {
            const id<MTL4ComputeCommandEncoder> encoder = Metal::Recording::ComputeEncoder(cmd);
            const Pitch pitch = PitchOf(image.GetFormat(), extent);
            for (u32 layer = 0; layer < layers; ++layer)
            {
                [encoder copyFromBuffer:buffer.GetNative().Buffer
                           sourceOffset:offset
                      sourceBytesPerRow:pitch.Row
                    sourceBytesPerImage:pitch.Slice
                             sourceSize:ToSize(extent)
                              toTexture:image.GetNative().Texture
                       destinationSlice:baseLayer + layer
                       destinationLevel:mip
                      destinationOrigin:ToOrigin(origin)];
                offset += pitch.Slice * extent.z;
            }
        }

        void CopyFromTexture(const CommandBuffer& cmd, const Image& image, const u32 mip,
                             const u32 baseLayer, const u32 layers, const uvec3 origin,
                             const uvec3 extent, const Buffer& buffer, u64 offset)
        {
            const id<MTL4ComputeCommandEncoder> encoder = Metal::Recording::ComputeEncoder(cmd);
            const Pitch pitch = PitchOf(image.GetFormat(), extent);
            for (u32 layer = 0; layer < layers; ++layer)
            {
                [encoder copyFromTexture:image.GetNative().Texture
                                 sourceSlice:baseLayer + layer
                                 sourceLevel:mip
                                sourceOrigin:ToOrigin(origin)
                                  sourceSize:ToSize(extent)
                                    toBuffer:buffer.GetNative().Buffer
                           destinationOffset:offset
                      destinationBytesPerRow:pitch.Row
                    destinationBytesPerImage:pitch.Slice];
                offset += pitch.Slice * extent.z;
            }
        }

        void CopyTextureRegion(const CommandBuffer& cmd, const Image& source, const u32 sourceMip,
                               const u32 sourceLayer, const uvec3 sourceOrigin,
                               const Image& destination, const u32 destinationMip,
                               const u32 destinationLayer, const uvec3 destinationOrigin,
                               const uvec3 extent)
        {
            [Metal::Recording::ComputeEncoder(cmd) copyFromTexture:source.GetNative().Texture
                                                       sourceSlice:sourceLayer
                                                       sourceLevel:sourceMip
                                                      sourceOrigin:ToOrigin(sourceOrigin)
                                                        sourceSize:ToSize(extent)
                                                         toTexture:destination.GetNative().Texture
                                                  destinationSlice:destinationLayer
                                                  destinationLevel:destinationMip
                                                 destinationOrigin:ToOrigin(destinationOrigin)];
        }

        // Sets the render encoder state a graphics pipeline carries beside its pipeline state.
        void ApplyGraphicsPipeline(const CommandBuffer::Native& native,
                                   const GraphicsPipeline& pipeline)
        {
            const GraphicsPipeline::Native& state = pipeline.GetNative();
            [native.Render setRenderPipelineState:state.State];
            [native.Render setDepthStencilState:state.DepthStencil];
            [native.Render setCullMode:state.Cull];
            [native.Render setTriangleFillMode:state.Fill];
            [native.Render setDepthClipMode:state.DepthClip];
        }

        // The bound index buffer from its index @p firstIndex on.
        struct IndexRange
        {
            u64 Address = 0;
            u64 Length = 0;
        };

        IndexRange IndexRangeOf(const CommandBuffer::Native& native, const u32 firstIndex)
        {
            VE_ASSERT(native.IndexBuffer, "CommandBuffer: an indexed draw with no index buffer");
            const u64 size = native.IndexType == MTLIndexTypeUInt16 ? 2 : 4;
            const u64 offset = u64{firstIndex} * size;
            VE_ASSERT(offset <= native.IndexBuffer->GetSize(),
                      "CommandBuffer: first index {} lies past the end of '{}'", firstIndex,
                      native.IndexBuffer->GetName());
            return {.Address = [native.IndexBuffer->GetNative().Buffer gpuAddress] + offset,
                    .Length = native.IndexBuffer->GetSize() - offset};
        }
    }

    namespace Metal::Recording
    {
        void OrderEncoder(CommandBuffer::Native& native, id<MTL4CommandEncoder> encoder,
                          const MetalStage encoderStages)
        {
            // Metal 4 orders no work across submissions on its own, so a recording begins as if a
            // barrier from everything earlier to every stage had been recorded.
            if (native.FirstEncoder)
            {
                native.PendingAfter = MetalStage::All;
                native.PendingBefore = MetalStage::All;
                native.FirstEncoder = false;
            }
            const MetalStage before = native.PendingBefore & encoderStages;
            if (before == MetalStage::None)
            {
                return;
            }
            // A consumer barrier waits on every earlier encoder's work, whichever kind it was; the
            // stages this encoder does not run stay pending for the next encoder that does.
            [encoder barrierAfterQueueStages:static_cast<MTLStages>(native.PendingAfter)
                                beforeStages:static_cast<MTLStages>(before)
                           visibilityOptions:MTL4VisibilityOptionDevice];
            native.PendingBefore = static_cast<MetalStage>(static_cast<u64>(native.PendingBefore) &
                                                           ~static_cast<u64>(encoderStages));
            if (native.PendingBefore == MetalStage::None)
            {
                native.PendingAfter = MetalStage::None;
            }
        }

        id<MTL4ComputeCommandEncoder> ComputeEncoder(const CommandBuffer& cmd)
        {
            CommandBuffer::Native& native = cmd.GetNative();
            VE_ASSERT(native.Recording, "CommandBuffer: recording into a buffer that is not begun");
            VE_ASSERT(native.Render == nil,
                      "CommandBuffer: compute or transfer work recorded inside a render pass");
            // Whatever is encoded next may write what a range's encode reads.
            native.IndirectCountOrdered = false;
            if (native.Compute != nil)
            {
                return native.Compute;
            }
            native.Compute = [native.CommandBuffer computeCommandEncoder];
            OrderEncoder(native, native.Compute, ComputeEncoderStages);
            [native.Compute setArgumentTable:native.ComputeTable];
            if (native.BoundCompute)
            {
                [native.Compute setComputePipelineState:native.BoundCompute->GetNative().State];
            }
            return native.Compute;
        }

        id<MTL4RenderCommandEncoder> RenderEncoder(const CommandBuffer& cmd,
                                                   MTL4RenderPassDescriptor* pass)
        {
            CommandBuffer::Native& native = cmd.GetNative();
            VE_ASSERT(native.Recording, "CommandBuffer: recording into a buffer that is not begun");
            VE_ASSERT(native.Render == nil, "CommandBuffer: a render pass is already open");
            EndEncoder(cmd);
            const id<MTL4RenderCommandEncoder> encoder =
                [native.CommandBuffer renderCommandEncoderWithDescriptor:pass];
            OrderEncoder(native, encoder, RenderEncoderStages);
            return encoder;
        }

        void EndEncoder(const CommandBuffer& cmd)
        {
            CommandBuffer::Native& native = cmd.GetNative();
            if (native.Compute != nil)
            {
                [native.Compute endEncoding];
                native.Compute = nil;
            }
        }

        void Barrier(const CommandBuffer& cmd, const MetalStage after, const MetalStage before)
        {
            if (after == MetalStage::None || before == MetalStage::None)
            {
                return;
            }
            CommandBuffer::Native& native = cmd.GetNative();
            VE_ASSERT(native.Render == nil,
                      "CommandBuffer: a barrier recorded inside a render pass");
            // Recorded as a consumer barrier on the encoders that follow, which is what orders
            // work a render pass produced as well as work a compute encoder did.
            EndEncoder(cmd);
            native.PendingAfter |= after;
            native.PendingBefore |= before;
        }

        void CopyBuffer(const CommandBuffer& cmd, const Buffer& source, const u64 sourceOffset,
                        const Buffer& destination, const u64 destinationOffset, const u64 size)
        {
            [ComputeEncoder(cmd) copyFromBuffer:source.GetNative().Buffer
                                   sourceOffset:sourceOffset
                                       toBuffer:destination.GetNative().Buffer
                              destinationOffset:destinationOffset
                                           size:size];
        }
    }

    CommandBuffer::Native& CommandBuffer::GetNative() const
    {
        return *m_Native;
    }

    CommandBuffer::CommandBuffer(Context& context, const CommandBufferLevel level)
        : m_Context(context), m_Level(level), m_Native(CreateUnique<Native>())
    {
        const id<MTLDevice> device = context.GetNative().Device;
        m_Native->CommandBuffer = [device newCommandBuffer];
        m_Native->Allocator = [device newCommandAllocator];
        VE_ASSERT(m_Native->CommandBuffer != nil && m_Native->Allocator != nil,
                  "CommandBuffer: failed to create a Metal command buffer and allocator");

        // Each stage's table ends at the last slot it binds: the push block for compute and
        // fragment, the instance buffer for vertex. A table spanning every slot with initialized
        // bindings leaves Metal's shader validation reporting nothing.
        namespace Slots = Cook::MetalBindingSlots;
        const auto makeTable = [device](const u32 slots, NSString* label)
        {
            MTL4ArgumentTableDescriptor* const table = [[MTL4ArgumentTableDescriptor alloc] init];
            table.maxBufferBindCount = slots;
            table.initializeBindings = YES;
            table.label = label;
            NSError* error = nil;
            const id<MTL4ArgumentTable> created = [device newArgumentTableWithDescriptor:table
                                                                                   error:&error];
            VE_ASSERT(created != nil, "CommandBuffer: failed to create an argument table: {}",
                      Metal::Describe(error));
            return created;
        };
        m_Native->ComputeTable = makeTable(Slots::PushConstants + 1, @"Compute Arguments");
        m_Native->VertexTable = makeTable(Slots::VertexBufferBase + 2, @"Vertex Arguments");
        m_Native->FragmentTable = makeTable(Slots::PushConstants + 1, @"Fragment Arguments");
    }

    CommandBuffer::~CommandBuffer()
    {
        Context::Native& native = m_Context.GetNative();
        native.ReleaseRingChunks(m_Native->RingChunks);
        for (const id<MTLIndirectCommandBuffer> commands : m_Native->IndirectCommandBuffers)
        {
            native.Retire(function<void()>([&native, commands] { native.Evict(commands); }));
            native.Retire(commands);
        }
    }

    void CommandBuffer::Reset()
    {
        // Begin resets the allocator, once the work this buffer recorded last has completed.
    }

    void CommandBuffer::Begin(CommandBufferUsage /*flags*/)
    {
        VE_ASSERT(!m_Native->Recording, "CommandBuffer::Begin: already recording");
        m_DrawCallCount = 0;
        [m_Native->Allocator reset];
        [m_Native->CommandBuffer beginCommandBufferWithAllocator:m_Native->Allocator];
        m_Native->Recording = true;
        m_Native->FirstEncoder = true;
        m_Native->PendingAfter = Metal::MetalStage::None;
        m_Native->PendingBefore = Metal::MetalStage::None;
        m_Native->BoundCompute = nullptr;
        m_Native->BoundGraphics = nullptr;
        m_Native->IndexBuffer = nullptr;
        m_Native->Ring.Rewind();
        m_Native->PushAddress = 0;
        m_Native->ComputePushBound = 0;
        m_Native->RenderPushBound = 0;
        m_Native->IndirectCountRanges.clear();
        m_Native->IndirectCommandBuffer = 0;
        m_Native->IndirectCommandCursor = 0;
        m_Native->IndirectCountOrdered = false;
        m_LastBoundPipelineLayout = nullptr;
        m_BoundGraphicsPipeline = nullptr;
        m_HasActiveRenderingInfo = false;
    }

    void CommandBuffer::End()
    {
        VE_ASSERT(m_Native->Render == nil, "CommandBuffer::End: a render pass is still open");
        Metal::Recording::EndEncoder(*this);
        [m_Native->CommandBuffer endCommandBuffer];
        m_Native->Recording = false;
    }

    void CommandBuffer::ValidateAttachmentFormats()
    {
#if defined(VE_DEBUG) && VE_DEBUG
        if (m_AttachmentFormatsValidated || !m_HasActiveRenderingInfo || !m_BoundGraphicsPipeline)
        {
            return;
        }
        m_AttachmentFormatsValidated = true;
        const vector<Format>& colors = m_BoundGraphicsPipeline->GetColorAttachmentFormats();
        VE_ASSERT(colors.size() == m_ActiveColorAttachmentFormats.size(),
                  "CommandBuffer::Draw: pipeline '{}' declares {} color attachment(s) but {} are "
                  "active",
                  m_BoundGraphicsPipeline->GetName(), colors.size(),
                  m_ActiveColorAttachmentFormats.size());
        for (usize i = 0; i < colors.size(); i++)
        {
            VE_ASSERT(colors[i] == m_ActiveColorAttachmentFormats[i],
                      "CommandBuffer::Draw: pipeline '{}' color attachment {} is {}, the render "
                      "target is {}",
                      m_BoundGraphicsPipeline->GetName(), i, FormatName(colors[i]),
                      FormatName(m_ActiveColorAttachmentFormats[i]));
        }
        VE_ASSERT(m_BoundGraphicsPipeline->GetDepthAttachmentFormat() ==
                      m_ActiveDepthAttachmentFormat,
                  "CommandBuffer::Draw: pipeline '{}' depth attachment is {}, the render target "
                  "is {}",
                  m_BoundGraphicsPipeline->GetName(),
                  FormatName(m_BoundGraphicsPipeline->GetDepthAttachmentFormat()),
                  FormatName(m_ActiveDepthAttachmentFormat));
#endif
    }

    void CommandBuffer::BeginRendering(const RenderingInfo& info)
    {
        VE_ASSERT(info.ViewMask == 0,
                  "CommandBuffer::BeginRendering: the Metal backend renders no multiview pass");
        m_ActiveColorAttachmentFormats.clear();
        m_ActiveColorAttachmentFormats.reserve(info.ColorAttachments.size());

        // A load action clears the whole attachment where Vulkan clears the render area alone, so
        // an attachment the area does not cover loads, and the clear is drawn over the area.
        const auto coversAttachment = [&info](const ImageView& view)
        {
            const uvec3 extent = view.GetImage()->GetExtent();
            const u32 mip = view.GetBaseMipLevel();
            const uvec2 full(std::max(extent.x >> mip, 1u), std::max(extent.y >> mip, 1u));
            return info.Offset == ivec2(0) && glm::all(glm::greaterThanEqual(info.Extent, full));
        };
        Metal::AreaClear areaClear;
        VE_ASSERT(info.ColorAttachments.size() <= Metal::MaxAreaClearAttachments,
                  "CommandBuffer::BeginRendering: {} colour attachments exceed the {}-slot limit",
                  info.ColorAttachments.size(), Metal::MaxAreaClearAttachments);

        MTL4RenderPassDescriptor* const pass = [[MTL4RenderPassDescriptor alloc] init];
        for (usize i = 0; i < info.ColorAttachments.size(); ++i)
        {
            const RenderingAttachmentInfo& attachment = info.ColorAttachments[i];
            m_ActiveColorAttachmentFormats.push_back(attachment.ImageView != nullptr
                                                         ? attachment.ImageView->GetFormat()
                                                         : Format::Undefined);
            if (attachment.ImageView == nullptr)
            {
                continue;
            }
            MTLRenderPassColorAttachmentDescriptor* const color = pass.colorAttachments[i];
            color.texture = attachment.ImageView->GetNative().Texture;
            color.loadAction = Metal::ToMetal(attachment.LoadOp);
            color.storeAction = Metal::ToMetal(attachment.StoreOp);
            areaClear.ColorFormats[i] = [color.texture pixelFormat];
            areaClear.SampleCount = [color.texture sampleCount];
            const auto* clear = std::get_if<ClearColor>(&attachment.ClearValue);
            if (clear != nullptr)
            {
                color.clearColor = MTLClearColorMake(clear->R, clear->G, clear->B, clear->A);
            }
            if (attachment.LoadOp == LoadOp::Clear && !coversAttachment(*attachment.ImageView))
            {
                color.loadAction = MTLLoadActionLoad;
                areaClear.ColorMask |= 1u << i;
                areaClear.Colors[i] =
                    clear != nullptr ? vec4(clear->R, clear->G, clear->B, clear->A) : vec4(0.0f);
            }
        }

        const auto bindDepthStencil =
            [pass, &areaClear, &coversAttachment](const RenderingAttachmentInfo& attachment,
                                                  const bool depth, const bool stencil)
        {
            const id<MTLTexture> texture = attachment.ImageView->GetNative().Texture;
            const auto* clear = std::get_if<ClearDepth>(&attachment.ClearValue);
            const bool partialClear =
                attachment.LoadOp == LoadOp::Clear && !coversAttachment(*attachment.ImageView);
            areaClear.SampleCount = [texture sampleCount];
            if (depth)
            {
                pass.depthAttachment.texture = texture;
                pass.depthAttachment.loadAction = Metal::ToMetal(attachment.LoadOp);
                pass.depthAttachment.storeAction = Metal::ToMetal(attachment.StoreOp);
                pass.depthAttachment.clearDepth = clear != nullptr ? clear->Depth : 1.0;
                if (partialClear)
                {
                    pass.depthAttachment.loadAction = MTLLoadActionLoad;
                    areaClear.ClearsDepth = true;
                    areaClear.Depth = clear != nullptr ? clear->Depth : 1.0f;
                }
            }
            if (stencil)
            {
                VE_ASSERT(!partialClear,
                          "CommandBuffer::BeginRendering: the Metal backend clears no stencil "
                          "over part of an attachment");
                pass.stencilAttachment.texture = texture;
                pass.stencilAttachment.loadAction = Metal::ToMetal(attachment.LoadOp);
                pass.stencilAttachment.storeAction = Metal::ToMetal(attachment.StoreOp);
                pass.stencilAttachment.clearStencil = clear != nullptr ? clear->Stencil : 0;
            }
        };
        m_ActiveDepthAttachmentFormat = Format::Undefined;
        if (info.DepthAttachment.has_value())
        {
            const Format format = info.DepthAttachment->ImageView->GetFormat();
            m_ActiveDepthAttachmentFormat = format;
            const bool withStencil = format == Format::D32SfloatS8Uint;
            bindDepthStencil(*info.DepthAttachment, true,
                             withStencil && !info.StencilAttachment.has_value());
        }
        if (info.StencilAttachment.has_value())
        {
            bindDepthStencil(*info.StencilAttachment, false, true);
        }

        const uvec2 extent = uvec2(info.Offset) + info.Extent;
        pass.renderTargetWidth = extent.x;
        pass.renderTargetHeight = extent.y;
        // A pass whose colour slots are all unbound and that has no depth names its sample count
        // here; Metal takes it from an attachment otherwise.
        pass.defaultRasterSampleCount = 1;
        if (info.LayerCount > 1)
        {
            pass.renderTargetArrayLength = info.LayerCount;
        }

        m_HasActiveRenderingInfo = true;
        m_AttachmentFormatsValidated = false;
        m_Native->RenderExtent = extent;
        m_Native->Render = Metal::Recording::RenderEncoder(*this, pass);
        [m_Native->Render setArgumentTable:m_Native->VertexTable atStages:MTLRenderStageVertex];
        [m_Native->Render setArgumentTable:m_Native->FragmentTable atStages:MTLRenderStageFragment];
        // The flipped viewport lands primitives where Vulkan does, and Metal judges their winding
        // there, so the engine's counter-clockwise front face carries over unchanged.
        [m_Native->Render setFrontFacingWinding:MTLWindingCounterClockwise];
        SetViewport(info.Offset, info.Extent);
        SetScissor(info.Offset, info.Extent);
        if (areaClear.IsNeeded())
        {
            VE_ASSERT(info.LayerCount <= 1,
                      "CommandBuffer::BeginRendering: the Metal backend clears no layered pass "
                      "over part of its attachments");
            Metal::ClearRenderArea(m_Context, *this, areaClear);
        }
        if (m_Native->BoundGraphics)
        {
            ApplyGraphicsPipeline(*m_Native, *m_Native->BoundGraphics);
        }
    }

    void CommandBuffer::EndRendering()
    {
        VE_ASSERT(m_Native->Render != nil, "CommandBuffer::EndRendering: no render pass is open");
        [m_Native->Render endEncoding];
        m_Native->Render = nil;
        m_ActiveColorAttachmentFormats.clear();
        m_ActiveDepthAttachmentFormat = Format::Undefined;
        m_HasActiveRenderingInfo = false;
        m_AttachmentFormatsValidated = false;
    }

    void CommandBuffer::PushConstants(const PushConstantsInfo& info)
    {
        VE_ASSERT(m_LastBoundPipelineLayout, "PushConstants: no pipeline layout is bound");
        m_Native->Push.Write(info.Offset,
                             std::span(static_cast<const std::byte*>(info.Data), info.Size));
    }

    void CommandBuffer::BindDescriptorSets(const DescriptorSetBindInfo& info)
    {
        VE_ASSERT(info.Sets.size() <= MaxBoundDescriptorSets,
                  "CommandBuffer::BindDescriptorSets: {} sets exceeds the {}-set limit",
                  info.Sets.size(), MaxBoundDescriptorSets);
        std::array<const DescriptorSet*, MaxBoundDescriptorSets> sets{};
        for (usize i = 0; i < info.Sets.size(); ++i)
        {
            sets[i] = info.Sets[i].get();
        }
        BindDescriptorSets(std::span<const DescriptorSet* const>(sets.data(), info.Sets.size()),
                           info.FirstSet, info.PipelineBindPoint, info.DynamicOffsets);
    }

    void CommandBuffer::BindDescriptorSets(const std::span<const DescriptorSet* const> sets,
                                           const u32 firstSet, const PipelineBindPoint bindPoint,
                                           const std::span<const u32> dynamicOffsets)
    {
        VE_ASSERT(m_LastBoundPipelineLayout,
                  "CommandBuffer::BindDescriptorSets: no pipeline layout is bound");
        VE_ASSERT(firstSet + sets.size() <= MaxBoundDescriptorSets,
                  "CommandBuffer::BindDescriptorSets: sets {}+{} exceed the {}-set limit", firstSet,
                  sets.size(), MaxBoundDescriptorSets);
        const PipelineLayout::Native& layout = m_LastBoundPipelineLayout->GetNative();
        // A bind point keeps its own bindings, as on every backend: the compute table, or the two
        // render stages' tables together.
        const bool compute = bindPoint == PipelineBindPoint::Compute;
        const auto setAddress = [this, compute](const u64 address, const u32 slot)
        {
            if (compute)
            {
                [m_Native->ComputeTable setAddress:address atIndex:slot];
                return;
            }
            [m_Native->VertexTable setAddress:address atIndex:slot];
            [m_Native->FragmentTable setAddress:address atIndex:slot];
        };
        usize nextDynamic = 0;
        for (usize i = 0; i < sets.size(); ++i)
        {
            const u32 set = firstSet + static_cast<u32>(i);
            const DescriptorSet::Native& native = sets[i]->GetNative();
            // Set N's argument buffer is bound at slot N.
            setAddress([native.Arguments->GetNative().Buffer gpuAddress],
                       Cook::MetalBindingSlots::DescriptorSetBase + set);
            for (usize d = 0; d < native.DynamicAddresses.size(); ++d)
            {
                VE_ASSERT(nextDynamic < dynamicOffsets.size(),
                          "CommandBuffer::BindDescriptorSets: set {} has a dynamic buffer and no "
                          "dynamic offset is left for it",
                          set);
                VE_ASSERT(set < layout.DynamicSlots.size(),
                          "CommandBuffer::BindDescriptorSets: set {} is past the bound layout '{}'",
                          set, m_LastBoundPipelineLayout->GetName());
                setAddress(native.DynamicAddresses[d] + dynamicOffsets[nextDynamic++],
                           layout.DynamicSlots[set] + static_cast<u32>(d));
            }
        }
        VE_ASSERT(nextDynamic == dynamicOffsets.size(),
                  "CommandBuffer::BindDescriptorSets: {} dynamic offsets for {} dynamic buffers",
                  dynamicOffsets.size(), nextDynamic);
    }

    // Copies the push block into the ring when it changed since its last copy.
    static void FlushPushConstants(Context& context, CommandBuffer::Native& native)
    {
        if (!native.Push.IsDirty())
        {
            return;
        }
        const std::span<const std::byte> bytes = native.Push.GetBytes();
        const Metal::RingPlacement placement = native.Ring.Allocate(static_cast<u32>(bytes.size()));
        while (placement.Chunk >= native.RingChunks.size())
        {
            native.RingChunks.push_back(context.GetNative().AcquireRingChunk(context));
        }
        const Buffer& chunk = *native.RingChunks[placement.Chunk];
        std::memcpy(static_cast<std::byte*>(chunk.GetNative().Contents) + placement.Offset,
                    bytes.data(), bytes.size());
        native.PushAddress = [chunk.GetNative().Buffer gpuAddress] + placement.Offset;
        native.Push.MarkClean();
    }

    // The open render encoder, with the push block's latest copy bound for this draw.
    static id<MTL4RenderCommandEncoder> PrepareDraw(Context& context, const CommandBuffer& cmd)
    {
        CommandBuffer::Native& native = cmd.GetNative();
        VE_ASSERT(native.Render != nil, "CommandBuffer: a draw recorded outside a render pass");
        VE_ASSERT(native.BoundGraphics, "CommandBuffer: a draw with no graphics pipeline bound");
        FlushPushConstants(context, native);
        if (native.PushAddress != 0 && native.RenderPushBound != native.PushAddress)
        {
            [native.VertexTable setAddress:native.PushAddress
                                   atIndex:Cook::MetalBindingSlots::PushConstants];
            [native.FragmentTable setAddress:native.PushAddress
                                     atIndex:Cook::MetalBindingSlots::PushConstants];
            native.RenderPushBound = native.PushAddress;
        }
        return native.Render;
    }

    void CommandBuffer::DrawFullscreenTriangle()
    {
        Draw(3, 1, 0, 0);
    }

    void CommandBuffer::Draw(const u32 vertexCount, const u32 instanceCount, const u32 firstVertex,
                             const u32 firstInstance)
    {
        const id<MTL4RenderCommandEncoder> encoder = PrepareDraw(m_Context, *this);
        ValidateAttachmentFormats();
        ++m_DrawCallCount;
        if (vertexCount == 0 || instanceCount == 0)
        {
            return;
        }
        [encoder drawPrimitives:m_Native->BoundGraphics->GetNative().Primitive
                    vertexStart:firstVertex
                    vertexCount:vertexCount
                  instanceCount:instanceCount
                   baseInstance:firstInstance];
    }

    void CommandBuffer::DrawIndexed(const u32 indexCount, const u32 instanceCount,
                                    const u32 firstIndex, const i32 vertexOffset,
                                    const u32 firstInstance)
    {
        const id<MTL4RenderCommandEncoder> encoder = PrepareDraw(m_Context, *this);
        ValidateAttachmentFormats();
        ++m_DrawCallCount;
        if (indexCount == 0 || instanceCount == 0)
        {
            return;
        }
        const IndexRange indices = IndexRangeOf(*m_Native, firstIndex);
        [encoder drawIndexedPrimitives:m_Native->BoundGraphics->GetNative().Primitive
                            indexCount:indexCount
                             indexType:m_Native->IndexType
                           indexBuffer:indices.Address
                     indexBufferLength:indices.Length
                         instanceCount:instanceCount
                            baseVertex:vertexOffset
                          baseInstance:firstInstance];
    }

    void CommandBuffer::DrawIndexedIndirect(const Ref<Buffer>& buffer, const u64 offset,
                                            const u32 drawCount, const u32 stride)
    {
        VE_ASSERT(offset % 4 == 0, "DrawIndexedIndirect: offset {} is not 4-byte aligned", offset);
        VE_ASSERT(drawCount <= 1 || stride >= sizeof(DrawIndexedIndirectCommand),
                  "DrawIndexedIndirect: stride {} is shorter than a record", stride);
        VE_ASSERT(offset + (drawCount == 0 ? 0
                                           : (u64{drawCount - 1} * stride) +
                                                 sizeof(DrawIndexedIndirectCommand)) <=
                      buffer->GetSize(),
                  "DrawIndexedIndirect: {} records at offset {} run past the end of '{}'",
                  drawCount, offset, buffer->GetName());
        const id<MTL4RenderCommandEncoder> encoder = PrepareDraw(m_Context, *this);
        ValidateAttachmentFormats();
        ++m_DrawCallCount;
        const IndexRange indices = IndexRangeOf(*m_Native, 0);
        const u64 base = [buffer->GetNative().Buffer gpuAddress] + offset;
        // Metal issues one indirect draw per call, so a multi-draw is one per record at its stride.
        for (u32 i = 0; i < drawCount; ++i)
        {
            [encoder drawIndexedPrimitives:m_Native->BoundGraphics->GetNative().Primitive
                                 indexType:m_Native->IndexType
                               indexBuffer:indices.Address
                         indexBufferLength:indices.Length
                            indirectBuffer:base + (u64{i} * stride)];
        }
    }

    void CommandBuffer::DrawIndirect(const Ref<Buffer>& buffer, const u64 offset,
                                     const u32 drawCount, const u32 stride)
    {
        VE_ASSERT(offset % 4 == 0, "DrawIndirect: offset {} is not 4-byte aligned", offset);
        VE_ASSERT(drawCount <= 1 || stride >= sizeof(DrawIndirectCommand),
                  "DrawIndirect: stride {} is shorter than a record", stride);
        VE_ASSERT(offset + (drawCount == 0
                                ? 0
                                : (u64{drawCount - 1} * stride) + sizeof(DrawIndirectCommand)) <=
                      buffer->GetSize(),
                  "DrawIndirect: {} records at offset {} run past the end of '{}'", drawCount,
                  offset, buffer->GetName());
        const id<MTL4RenderCommandEncoder> encoder = PrepareDraw(m_Context, *this);
        ValidateAttachmentFormats();
        ++m_DrawCallCount;
        const u64 base = [buffer->GetNative().Buffer gpuAddress] + offset;
        for (u32 i = 0; i < drawCount; ++i)
        {
            [encoder drawPrimitives:m_Native->BoundGraphics->GetNative().Primitive
                     indirectBuffer:base + (u64{i} * stride)];
        }
    }

    namespace
    {
        // Places @p commands consecutive commands in the recording's indirect command buffers,
        // creating one that holds them when none has room.
        std::pair<id<MTLIndirectCommandBuffer>, u32>
        PlaceIndirectCommands(Context::Native& context, CommandBuffer::Native& native,
                              const u32 commands)
        {
            constexpr u32 MinimumCapacity = 4096;
            while (native.IndirectCommandBuffer < native.IndirectCommandBuffers.size())
            {
                const u32 capacity = native.IndirectCommandCapacities[native.IndirectCommandBuffer];
                if (native.IndirectCommandCursor + commands <= capacity)
                {
                    const u32 first = native.IndirectCommandCursor;
                    native.IndirectCommandCursor += commands;
                    return {native.IndirectCommandBuffers[native.IndirectCommandBuffer], first};
                }
                ++native.IndirectCommandBuffer;
                native.IndirectCommandCursor = 0;
            }
            // Draws inherit everything but the draw itself from the render encoder they execute in.
            MTLIndirectCommandBufferDescriptor* const descriptor =
                [[MTLIndirectCommandBufferDescriptor alloc] init];
            descriptor.commandTypes =
                MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed;
            descriptor.inheritPipelineState = YES;
            descriptor.inheritBuffers = YES;
            descriptor.inheritDepthStencilState = YES;
            descriptor.inheritDepthBias = YES;
            descriptor.inheritDepthClipMode = YES;
            descriptor.inheritCullMode = YES;
            descriptor.inheritFrontFacingWinding = YES;
            descriptor.inheritTriangleFillMode = YES;
            const u32 capacity = std::max(MinimumCapacity, commands);
            const id<MTLIndirectCommandBuffer> created = [context.Device
                newIndirectCommandBufferWithDescriptor:descriptor
                                       maxCommandCount:capacity
                                               options:MTLResourceStorageModePrivate |
                                                       MTLResourceHazardTrackingModeUntracked];
            VE_ASSERT(created != nil,
                      "CommandBuffer: failed to create an indirect command buffer of {} commands",
                      capacity);
            [created setLabel:@"Indirect Count Commands"];
            context.MakeResident(created);
            native.IndirectCommandBuffers.push_back(created);
            native.IndirectCommandCapacities.push_back(capacity);
            native.IndirectCommandBuffer =
                static_cast<u32>(native.IndirectCommandBuffers.size() - 1);
            native.IndirectCommandCursor = commands;
            return {created, 0};
        }

        // The prepared range a count-driven draw executes, completed with the draw's index buffer
        // and primitive.
        const CommandBuffer::Native::IndirectCountRange&
        ConsumeIndirectCount(const CommandBuffer::Native& native, const Buffer& buffer,
                             const u64 offset, const Buffer& countBuffer, const u64 countOffset,
                             const u32 indexType, const u64 indices, const u32 primitive)
        {
            const Metal::IndirectCountKey key{
                .Records = [buffer.GetNative().Buffer gpuAddress] + offset,
                .Count = [countBuffer.GetNative().Buffer gpuAddress] + countOffset};
            const auto found = native.IndirectCountRanges.find(key);
            VE_ASSERT(
                found != native.IndirectCountRanges.end(),
                "CommandBuffer: a count-driven draw of '{}' at {} with no PrepareIndirectCount "
                "for it earlier in the recording",
                buffer.GetName(), offset);
            Metal::IndirectCountParams& params = *found->second.Params;
            // The encode runs at execution, after every draw of the recording is recorded.
            VE_ASSERT(params.Primitive == Metal::IndirectCountUndrawn ||
                          (params.Primitive == primitive && params.IndexType == indexType &&
                           params.Indices == indices),
                      "CommandBuffer: the range of '{}' at {} is drawn twice with different "
                      "indices or primitives",
                      buffer.GetName(), offset);
            params.Primitive = primitive;
            params.IndexType = indexType;
            params.Indices = indices;
            return found->second;
        }
    }

    void CommandBuffer::PrepareIndirectCount(const Ref<Buffer>& buffer, const u64 offset,
                                             const Ref<Buffer>& countBuffer, const u64 countOffset,
                                             const u32 maxDraws, const u32 stride)
    {
        VE_ASSERT(offset % 4 == 0 && countOffset % 4 == 0,
                  "PrepareIndirectCount: offsets {} and {} are not 4-byte aligned", offset,
                  countOffset);
        VE_ASSERT(stride >= sizeof(DrawIndirectCommand) && stride % 4 == 0,
                  "PrepareIndirectCount: stride {} is not a record's", stride);
        VE_ASSERT(maxDraws == 0 ||
                      offset + (u64{maxDraws - 1} * stride) + sizeof(DrawIndirectCommand) <=
                          buffer->GetSize(),
                  "PrepareIndirectCount: {} records at offset {} run past the end of '{}'",
                  maxDraws, offset, buffer->GetName());
        Context::Native& context = m_Context.GetNative();
        Native& native = *m_Native;
        const Metal::IndirectCountKey key{
            .Records = [buffer->GetNative().Buffer gpuAddress] + offset,
            .Count = [countBuffer->GetNative().Buffer gpuAddress] + countOffset};
        VE_ASSERT(!native.IndirectCountRanges.contains(key),
                  "PrepareIndirectCount: the range of '{}' at {} is already prepared",
                  buffer->GetName(), offset);

        const auto [commands, first] =
            PlaceIndirectCommands(context, native, std::max(maxDraws, 1u));

        const Metal::RingPlacement placement =
            native.Ring.Allocate(static_cast<u32>(sizeof(Metal::IndirectCountParams)));
        while (placement.Chunk >= native.RingChunks.size())
        {
            native.RingChunks.push_back(context.AcquireRingChunk(m_Context));
        }
        const Buffer& chunk = *native.RingChunks[placement.Chunk];
        auto* const params = reinterpret_cast<Metal::IndirectCountParams*>(
            static_cast<std::byte*>(chunk.GetNative().Contents) + placement.Offset);
        const MTLResourceID commandsId = [commands gpuResourceID];
        u64 commandsBits = 0;
        static_assert(sizeof(commandsId) == sizeof(commandsBits));
        std::memcpy(&commandsBits, &commandsId, sizeof(commandsBits));
        *params = Metal::IndirectCountParams{
            .Commands = commandsBits,
            .Records = key.Records,
            .Count = key.Count,
            .Stride = stride,
            .MaxDraws = maxDraws,
            .First = first,
            .Primitive = Metal::IndirectCountUndrawn,
        };
        const u64 paramsAddress = [chunk.GetNative().Buffer gpuAddress] + placement.Offset;
        native.IndirectCountRanges.emplace(
            key, Native::IndirectCountRange{
                     .Commands = commands, .Params = params, .ParamsAddress = paramsAddress});

        if (native.IndirectCountTable == nil)
        {
            MTL4ArgumentTableDescriptor* const table = [[MTL4ArgumentTableDescriptor alloc] init];
            table.maxBufferBindCount = 1;
            table.initializeBindings = YES;
            table.label = @"Indirect Count Arguments";
            NSError* error = nil;
            native.IndirectCountTable = [context.Device newArgumentTableWithDescriptor:table
                                                                                 error:&error];
            VE_ASSERT(native.IndirectCountTable != nil,
                      "CommandBuffer: failed to create the indirect-count argument table: {}",
                      Metal::Describe(error));
        }

        const bool ordered = native.IndirectCountOrdered;
        const id<MTL4ComputeCommandEncoder> encoder = Metal::Recording::ComputeEncoder(*this);
        // The encode reads what the dispatches before it in this encoder wrote; encodes need no
        // order among themselves.
        if (!ordered)
        {
            [encoder barrierAfterEncoderStages:MTLStageDispatch | MTLStageBlit
                           beforeEncoderStages:MTLStageDispatch
                             visibilityOptions:MTL4VisibilityOptionDevice];
        }
        [native.IndirectCountTable setAddress:paramsAddress atIndex:0];
        [encoder setArgumentTable:native.IndirectCountTable];
        [encoder setComputePipelineState:context.IndirectCountPipeline()];
        const u32 groups = (std::max(maxDraws, 1u) + Metal::IndirectCountThreadgroupSize - 1) /
                           Metal::IndirectCountThreadgroupSize;
        [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(Metal::IndirectCountThreadgroupSize, 1, 1)];
        [encoder setArgumentTable:native.ComputeTable];
        if (native.BoundCompute)
        {
            [encoder setComputePipelineState:native.BoundCompute->GetNative().State];
        }
        native.IndirectCountOrdered = true;
    }

    void CommandBuffer::DrawIndexedIndirectCount(const Ref<Buffer>& buffer, const u64 offset,
                                                 const Ref<Buffer>& countBuffer,
                                                 const u64 countOffset, const u32 /*maxDraws*/,
                                                 const u32 /*stride*/)
    {
        const id<MTL4RenderCommandEncoder> encoder = PrepareDraw(m_Context, *this);
        ValidateAttachmentFormats();
        ++m_DrawCallCount;
        const IndexRange indices = IndexRangeOf(*m_Native, 0);
        const Native::IndirectCountRange& range = ConsumeIndirectCount(
            *m_Native, *buffer, offset, *countBuffer, countOffset,
            m_Native->IndexType == MTLIndexTypeUInt16 ? 1u : 2u, indices.Address,
            static_cast<u32>(m_Native->BoundGraphics->GetNative().Primitive));
        [encoder executeCommandsInBuffer:range.Commands
                          indirectBuffer:range.ParamsAddress +
                                         offsetof(Metal::IndirectCountParams, RangeLocation)];
    }

    void CommandBuffer::DrawIndirectCount(const Ref<Buffer>& buffer, const u64 offset,
                                          const Ref<Buffer>& countBuffer, const u64 countOffset,
                                          const u32 /*maxDraws*/, const u32 /*stride*/)
    {
        const id<MTL4RenderCommandEncoder> encoder = PrepareDraw(m_Context, *this);
        ValidateAttachmentFormats();
        ++m_DrawCallCount;
        const Native::IndirectCountRange& range =
            ConsumeIndirectCount(*m_Native, *buffer, offset, *countBuffer, countOffset, 0u, 0u,
                                 static_cast<u32>(m_Native->BoundGraphics->GetNative().Primitive));
        [encoder executeCommandsInBuffer:range.Commands
                          indirectBuffer:range.ParamsAddress +
                                         offsetof(Metal::IndirectCountParams, RangeLocation)];
    }

    static void BindComputePushConstants(Context& context, CommandBuffer::Native& native)
    {
        FlushPushConstants(context, native);
        if (native.PushAddress != 0 && native.ComputePushBound != native.PushAddress)
        {
            [native.ComputeTable setAddress:native.PushAddress
                                    atIndex:Cook::MetalBindingSlots::PushConstants];
            native.ComputePushBound = native.PushAddress;
        }
    }

    void CommandBuffer::Dispatch(const u32 groupsX, const u32 groupsY, const u32 groupsZ)
    {
        VE_ASSERT(m_Native->BoundCompute, "CommandBuffer::Dispatch: no compute pipeline is bound");
        BindComputePushConstants(m_Context, *m_Native);
        [Metal::Recording::ComputeEncoder(*this)
             dispatchThreadgroups:MTLSizeMake(groupsX, groupsY, groupsZ)
            threadsPerThreadgroup:m_Native->BoundCompute->GetNative().ThreadsPerThreadgroup];
    }

    void CommandBuffer::DispatchIndirect(const Ref<Buffer>& buffer, const u64 offset)
    {
        constexpr u64 RecordBytes = sizeof(DispatchIndirectCommand);
        VE_ASSERT(offset % 4 == 0, "DispatchIndirect: offset {} is not 4-byte aligned", offset);
        VE_ASSERT(offset + RecordBytes <= buffer->GetSize(),
                  "DispatchIndirect: the record at offset {} runs past the end of '{}' ({} bytes)",
                  offset, buffer->GetName(), buffer->GetSize());
        VE_ASSERT(m_Native->BoundCompute,
                  "CommandBuffer::DispatchIndirect: no compute pipeline is bound");
        BindComputePushConstants(m_Context, *m_Native);
        [Metal::Recording::ComputeEncoder(*this)
            dispatchThreadgroupsWithIndirectBuffer:[buffer->GetNative().Buffer gpuAddress] + offset
                             threadsPerThreadgroup:m_Native->BoundCompute->GetNative()
                                                       .ThreadsPerThreadgroup];
    }

    void CommandBuffer::BindPipeline(const Ref<GraphicsPipeline>& pipeline)
    {
        m_Native->BoundGraphics = pipeline;
        m_LastBoundPipelineLayout = pipeline->GetPipelineLayout();
        m_BoundGraphicsPipeline = pipeline;
        m_AttachmentFormatsValidated = false;
        m_Native->Push.SetSize(m_LastBoundPipelineLayout->GetNative().PushBytes);
        if (m_Native->Render != nil)
        {
            ApplyGraphicsPipeline(*m_Native, *pipeline);
        }
    }

    void CommandBuffer::BindPipeline(const Ref<ComputePipeline>& pipeline)
    {
        m_Native->BoundCompute = pipeline;
        m_LastBoundPipelineLayout = pipeline->GetPipelineLayout();
        m_BoundGraphicsPipeline = nullptr;
        m_AttachmentFormatsValidated = false;
        m_Native->Push.SetSize(m_LastBoundPipelineLayout->GetNative().PushBytes);
        if (m_Native->Compute != nil)
        {
            [m_Native->Compute setComputePipelineState:pipeline->GetNative().State];
        }
    }

    void CommandBuffer::SetScissor(const ivec2 offset, const uvec2 extent)
    {
        VE_ASSERT(m_Native->Render != nil, "SetScissor: no render pass is open");
        // Metal refuses a scissor reaching past the render target, which Vulkan clips.
        const uvec2 limit = m_Native->RenderExtent;
        const uvec2 origin = glm::min(uvec2(glm::max(offset, ivec2(0))), limit);
        const uvec2 end = glm::min(uvec2(glm::max(offset + ivec2(extent), ivec2(0))), limit);
        [m_Native->Render setScissorRect:MTLScissorRect{.x = origin.x,
                                                        .y = origin.y,
                                                        .width = end.x - origin.x,
                                                        .height = end.y - origin.y}];
    }

    void CommandBuffer::SetBlendConstants(const vec4 constants)
    {
        VE_ASSERT(m_Native->Render != nil, "SetBlendConstants: no render pass is open");
        [m_Native->Render setBlendColorRed:constants.r
                                     green:constants.g
                                      blue:constants.b
                                     alpha:constants.a];
    }

    void CommandBuffer::SetViewport(const ivec2 offset, const uvec2 extent)
    {
        VE_ASSERT(m_Native->Render != nil, "SetViewport: no render pass is open");
        // The engine's clip space is Vulkan's, with +Y down the target; a viewport of negative
        // height lands it the same way under Metal's +Y-up convention.
        [m_Native->Render setViewport:MTLViewport{
                                          .originX = static_cast<f64>(offset.x),
                                          .originY = static_cast<f64>(offset.y) + extent.y,
                                          .width = static_cast<f64>(extent.x),
                                          .height = -static_cast<f64>(extent.y),
                                          .znear = 0.0,
                                          .zfar = 1.0,
                                      }];
    }

    void CommandBuffer::BindVertexBuffer(const Ref<Buffer>& buffer)
    {
        [m_Native->VertexTable setAddress:[buffer->GetNative().Buffer gpuAddress]
                                  atIndex:Cook::MetalBindingSlots::VertexBufferBase];
    }

    void CommandBuffer::BindInstanceBuffer(const Ref<Buffer>& buffer)
    {
        [m_Native->VertexTable setAddress:[buffer->GetNative().Buffer gpuAddress]
                                  atIndex:Cook::MetalBindingSlots::VertexBufferBase + 1];
    }

    void CommandBuffer::BindIndexBuffer(const Ref<Buffer>& buffer, const IndexType type)
    {
        m_Native->IndexBuffer = buffer;
        m_Native->IndexType = Metal::ToMetal(type);
    }

    void CommandBuffer::CopyBufferToImage(const Ref<Buffer>& buffer, const Ref<Image>& image)
    {
        CopyToTexture(*this, *buffer, 0, *image, 0, 0,
                      image->GetType() == ImageType::Type3D ? 1 : image->GetLayers(), {0, 0, 0},
                      MipExtent(*image, 0));
    }

    void CommandBuffer::CopyBufferToImage(const Ref<Buffer>& buffer, const Ref<Image>& image,
                                          const std::span<const BufferImageCopyRegion> regions)
    {
        for (const BufferImageCopyRegion& region : regions)
        {
            CopyToTexture(*this, *buffer, region.BufferOffset, *image, region.MipLevel,
                          region.BaseArrayLayer, RegionLayers(*image, region), region.ImageOffset,
                          region.Extent);
        }
    }

    void CommandBuffer::CopyImageToBuffer(const Ref<Image>& image, const Ref<Buffer>& buffer)
    {
        CopyFromTexture(*this, *image, 0, 0, 1, {0, 0, 0}, MipExtent(*image, 0), *buffer, 0);
    }

    void CommandBuffer::CopyImageToBuffer(const Ref<Image>& image, const Ref<Buffer>& buffer,
                                          const std::span<const BufferImageCopyRegion> regions)
    {
        for (const BufferImageCopyRegion& region : regions)
        {
            CopyFromTexture(*this, *image, region.MipLevel, region.BaseArrayLayer,
                            RegionLayers(*image, region), region.ImageOffset, region.Extent,
                            *buffer, region.BufferOffset);
        }
    }

    void CommandBuffer::CopyImageRegionToBuffer(const Ref<Image>& image, const Ref<Buffer>& buffer,
                                                const uvec2 offset, const uvec2 extent)
    {
        CopyFromTexture(*this, *image, 0, 0, 1, {offset.x, offset.y, 0}, {extent.x, extent.y, 1},
                        *buffer, 0);
    }

    void CommandBuffer::CopyImageSubresourceToBuffer(const Ref<Image>& image,
                                                     const Ref<Buffer>& buffer, const u32 mipLevel,
                                                     const u32 arrayLayer)
    {
        CopyFromTexture(*this, *image, mipLevel, arrayLayer, 1, {0, 0, 0},
                        MipExtent(*image, mipLevel), *buffer, 0);
    }

    void CommandBuffer::CopyImage(const Ref<Image>& source, const Ref<Image>& destination)
    {
        VE_ASSERT(source->GetExtent() == destination->GetExtent(),
                  "CopyImage requires source and destination of equal extent");
        CopyTextureRegion(*this, *source, 0, 0, {0, 0, 0}, *destination, 0, 0, {0, 0, 0},
                          MipExtent(*source, 0));
    }

    void CommandBuffer::CopyImageLayers(const Ref<Image>& source, const Ref<Image>& destination,
                                        const std::span<const ImageLayerCopy> copies)
    {
        VE_ASSERT(source->GetExtent() == destination->GetExtent(),
                  "CopyImageLayers requires source and destination of equal extent");
        for (const ImageLayerCopy& copy : copies)
        {
            VE_ASSERT(copy.SourceLayer + copy.LayerCount <= source->GetLayers() &&
                          copy.DestinationLayer + copy.LayerCount <= destination->GetLayers(),
                      "CopyImageLayers: a layer range exceeds its image");
            for (u32 layer = 0; layer < copy.LayerCount; ++layer)
            {
                CopyTextureRegion(*this, *source, 0, copy.SourceLayer + layer, {0, 0, 0},
                                  *destination, 0, copy.DestinationLayer + layer, {0, 0, 0},
                                  MipExtent(*source, 0));
            }
        }
    }

    void CommandBuffer::CopyImageRegions(const Ref<Image>& source, const Ref<Image>& destination,
                                         const std::span<const ImageCopyRegion> regions)
    {
        for (const ImageCopyRegion& region : regions)
        {
            for (u32 layer = 0; layer < region.LayerCount; ++layer)
            {
                CopyTextureRegion(*this, *source, region.SourceMipLevel,
                                  region.SourceBaseLayer + layer, region.SourceOffset, *destination,
                                  region.DestinationMipLevel, region.DestinationBaseLayer + layer,
                                  region.DestinationOffset, region.Extent);
            }
        }
    }

    void CommandBuffer::ClearColorImage(const Ref<Image>& image, const vec4 value)
    {
        // A colour clear is a render pass's load action, one pass per mip and slice.
        const id<MTLTexture> texture = image->GetNative().Texture;
        for (u32 mip = 0; mip < image->GetMipLevels(); ++mip)
        {
            const u32 slices = image->GetType() == ImageType::Type3D ? MipExtent(*image, mip).z
                                                                     : image->GetLayers();
            for (u32 slice = 0; slice < slices; ++slice)
            {
                MTL4RenderPassDescriptor* const pass = [[MTL4RenderPassDescriptor alloc] init];
                MTLRenderPassColorAttachmentDescriptor* const attachment = pass.colorAttachments[0];
                attachment.texture = texture;
                attachment.level = mip;
                if (image->GetType() == ImageType::Type3D)
                {
                    attachment.depthPlane = slice;
                }
                else
                {
                    attachment.slice = slice;
                }
                attachment.loadAction = MTLLoadActionClear;
                attachment.storeAction = MTLStoreActionStore;
                attachment.clearColor = MTLClearColorMake(value.x, value.y, value.z, value.w);
                [Metal::Recording::RenderEncoder(*this, pass) endEncoding];
            }
        }
    }

    void CommandBuffer::FillBuffer(const Ref<Buffer>& buffer, const u32 value)
    {
        const u8 byte = static_cast<u8>(value);
        const bool uniform = value == byte * 0x01010101u;
        if (uniform)
        {
            [Metal::Recording::ComputeEncoder(*this) fillBuffer:buffer->GetNative().Buffer
                                                          range:NSMakeRange(0, buffer->GetSize())
                                                          value:byte];
            return;
        }
        // Metal fills bytes; a word pattern is copied from a staging buffer that carries it.
        const Ref<Buffer> pattern =
            Buffer::Create(m_Context, {
                                          .Name = buffer->GetName() + " (Fill)",
                                          .Size = buffer->GetSize(),
                                          .Usage = BufferUsage::TransferSrc,
                                      });
        auto* words = static_cast<u32*>(pattern->GetNative().Contents);
        std::fill_n(words, buffer->GetSize() / sizeof(u32), value);
        Metal::Recording::CopyBuffer(*this, *pattern, 0, *buffer, 0,
                                     buffer->GetSize() / sizeof(u32) * sizeof(u32));
    }

    void CommandBuffer::BlitImage(const BlitImageInfo& info)
    {
        // The offsets bound each region, start to end, as Vulkan's blit takes them.
        const ivec3 sourceExtent = info.SourceExtent - info.SourceOffset;
        const ivec3 destinationExtent = info.DestinationExtent - info.DestinationOffset;
        if (sourceExtent == destinationExtent)
        {
            // An unscaled blit samples every texel at its centre, so the filter changes nothing.
            for (u32 layer = 0; layer < info.LayerCount; ++layer)
            {
                CopyTextureRegion(*this, *info.SourceImage, info.SourceMipLevel,
                                  info.SourceBaseLayer + layer, uvec3(info.SourceOffset),
                                  *info.DestinationImage, info.DestinationMipLevel,
                                  info.DestinationBaseLayer + layer, uvec3(info.DestinationOffset),
                                  uvec3(sourceExtent));
            }
            return;
        }
        Metal::BlitScaled(m_Context, *this, info);
    }

    void CommandBuffer::PrepareForAccess(const Ref<ImageView>& view, const AccessKind kind)
    {
        // The render graph's own path for a declared access, so an out-of-graph use leaves the
        // image's tracked state exactly as a graph pass would.
        const auto scope = ScopeFor(kind);
        Backend::TransitionImage(*this, *view->GetImage(), scope.Layout, scope.Stage, scope.Access,
                                 view->GetBaseArrayLayer(), view->GetArrayLayers(),
                                 view->GetBaseMipLevel(), view->GetMipLevels());
    }

    void CommandBuffer::PrepareForAccess(const Ref<Buffer>& buffer,
                                         const std::initializer_list<AccessKind> previous,
                                         const std::initializer_list<AccessKind> next)
    {
        const auto fold = [](const std::initializer_list<AccessKind> kinds)
        {
            SubresourceState folded;
            for (const AccessKind kind : kinds)
            {
                VE_ASSERT(IsBufferAccess(kind) || kind == AccessKind::TransferSrc ||
                              kind == AccessKind::TransferDst,
                          "PrepareForAccess: access kind {} is not a buffer access",
                          static_cast<u32>(kind));
                const SubresourceState scope = ScopeFor(kind);
                folded.Stage |= scope.Stage;
                folded.Access |= scope.Access;
            }
            return folded;
        };
        const SubresourceState src = fold(previous);
        const SubresourceState dst = fold(next);
        Backend::TransitionBuffer(*this, *buffer, src.Stage, src.Access, dst.Stage, dst.Access);
    }
}
