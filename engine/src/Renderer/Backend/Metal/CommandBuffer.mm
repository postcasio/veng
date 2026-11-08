#include <Veng/Renderer/CommandBuffer.h>

#include <algorithm>
#include <cstring>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/FormatInfo.h>

#include "Barrier.h"
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

        [[noreturn]] void NoRenderPasses(const string_view call)
        {
            VE_ASSERT(
                false,
                "CommandBuffer::{}: the Metal backend records compute and transfer work only; "
                "it opens no render pass",
                call);
            std::abort();
        }
    }

    namespace Metal::Recording
    {
        id<MTL4ComputeCommandEncoder> ComputeEncoder(const CommandBuffer& cmd)
        {
            CommandBuffer::Native& native = cmd.GetNative();
            VE_ASSERT(native.Recording, "CommandBuffer: recording into a buffer that is not begun");
            if (native.Compute != nil)
            {
                return native.Compute;
            }
            native.Compute = [native.CommandBuffer computeCommandEncoder];
            if (native.FirstEncoder)
            {
                // Metal 4 orders no work across submissions on its own; the first encoder of a
                // recording waits for everything committed before it.
                [native.Compute barrierAfterQueueStages:MTLStageAll
                                           beforeStages:MTLStageAll
                                      visibilityOptions:MTL4VisibilityOptionDevice];
                native.FirstEncoder = false;
            }
            [native.Compute setArgumentTable:native.ComputeTable];
            if (native.BoundCompute)
            {
                [native.Compute setComputePipelineState:native.BoundCompute->GetNative().State];
            }
            return native.Compute;
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
            // A producer barrier orders every later encoder's work, which an encoder barrier or a
            // consumer barrier, each scoped to one encoder, does not.
            [ComputeEncoder(cmd) barrierAfterStages:static_cast<MTLStages>(after)
                                  beforeQueueStages:static_cast<MTLStages>(before)
                                  visibilityOptions:MTL4VisibilityOptionDevice];
            EndEncoder(cmd);
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

        MTL4ArgumentTableDescriptor* const table = [[MTL4ArgumentTableDescriptor alloc] init];
        // A compute stage binds no vertex buffers, so its table ends at the push-constant slot. A table
        // spanning every slot with initialized bindings leaves Metal's shader validation reporting
        // nothing.
        table.maxBufferBindCount = Cook::MetalBindingSlots::PushConstants + 1;
        table.initializeBindings = YES;
        NSError* error = nil;
        m_Native->ComputeTable = [device newArgumentTableWithDescriptor:table error:&error];
        VE_ASSERT(m_Native->ComputeTable != nil,
                  "CommandBuffer: failed to create an argument table: {}", Metal::Describe(error));
    }

    CommandBuffer::~CommandBuffer()
    {
        m_Context.GetNative().ReleaseRingChunks(m_Native->RingChunks);
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
        m_Native->BoundCompute = nullptr;
        m_Native->Ring.Rewind();
        m_LastBoundPipelineLayout = nullptr;
    }

    void CommandBuffer::End()
    {
        Metal::Recording::EndEncoder(*this);
        [m_Native->CommandBuffer endCommandBuffer];
        m_Native->Recording = false;
    }

    void CommandBuffer::ValidateAttachmentFormats() {}

    void CommandBuffer::BeginRendering(const RenderingInfo& /*info*/)
    {
        NoRenderPasses("BeginRendering");
    }

    void CommandBuffer::EndRendering()
    {
        NoRenderPasses("EndRendering");
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
        if (bindPoint != PipelineBindPoint::Compute)
        {
            NoRenderPasses("BindDescriptorSets (graphics)");
        }

        const PipelineLayout::Native& layout = m_LastBoundPipelineLayout->GetNative();
        const id<MTL4ArgumentTable> table = m_Native->ComputeTable;
        usize nextDynamic = 0;
        for (usize i = 0; i < sets.size(); ++i)
        {
            const u32 set = firstSet + static_cast<u32>(i);
            const DescriptorSet::Native& native = sets[i]->GetNative();
            // Set N's argument buffer is bound at slot N.
            [table setAddress:[native.Arguments->GetNative().Buffer gpuAddress]
                      atIndex:Cook::MetalBindingSlots::DescriptorSetBase + set];
            for (usize d = 0; d < native.DynamicAddresses.size(); ++d)
            {
                VE_ASSERT(nextDynamic < dynamicOffsets.size(),
                          "CommandBuffer::BindDescriptorSets: set {} has a dynamic buffer and no "
                          "dynamic offset is left for it",
                          set);
                VE_ASSERT(set < layout.DynamicSlots.size(),
                          "CommandBuffer::BindDescriptorSets: set {} is past the bound layout '{}'",
                          set, m_LastBoundPipelineLayout->GetName());
                [table setAddress:native.DynamicAddresses[d] + dynamicOffsets[nextDynamic++]
                          atIndex:layout.DynamicSlots[set] + static_cast<u32>(d)];
            }
        }
        VE_ASSERT(nextDynamic == dynamicOffsets.size(),
                  "CommandBuffer::BindDescriptorSets: {} dynamic offsets for {} dynamic buffers",
                  dynamicOffsets.size(), nextDynamic);
    }

    void CommandBuffer::DrawFullscreenTriangle()
    {
        NoRenderPasses("DrawFullscreenTriangle");
    }

    void CommandBuffer::Draw(u32, u32, u32, u32)
    {
        NoRenderPasses("Draw");
    }

    void CommandBuffer::DrawIndexed(u32, u32, u32, i32, u32)
    {
        NoRenderPasses("DrawIndexed");
    }

    void CommandBuffer::DrawIndexedIndirect(const Ref<Buffer>&, u64, u32, u32)
    {
        NoRenderPasses("DrawIndexedIndirect");
    }

    void CommandBuffer::DrawIndirect(const Ref<Buffer>&, u64, u32, u32)
    {
        NoRenderPasses("DrawIndirect");
    }

    // Copies the push block into the ring and binds the copy, when it changed since the last.
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
        [native.ComputeTable setAddress:[chunk.GetNative().Buffer gpuAddress] + placement.Offset
                                atIndex:Cook::MetalBindingSlots::PushConstants];
        native.Push.MarkClean();
    }

    void CommandBuffer::Dispatch(const u32 groupsX, const u32 groupsY, const u32 groupsZ)
    {
        VE_ASSERT(m_Native->BoundCompute, "CommandBuffer::Dispatch: no compute pipeline is bound");
        FlushPushConstants(m_Context, *m_Native);
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
        FlushPushConstants(m_Context, *m_Native);
        [Metal::Recording::ComputeEncoder(*this)
            dispatchThreadgroupsWithIndirectBuffer:[buffer->GetNative().Buffer gpuAddress] + offset
                             threadsPerThreadgroup:m_Native->BoundCompute->GetNative()
                                                       .ThreadsPerThreadgroup];
    }

    void CommandBuffer::BindPipeline(const Ref<GraphicsPipeline>& /*pipeline*/)
    {
        NoRenderPasses("BindPipeline (graphics)");
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

    void CommandBuffer::SetScissor(ivec2, uvec2)
    {
        NoRenderPasses("SetScissor");
    }

    void CommandBuffer::SetBlendConstants(vec4)
    {
        NoRenderPasses("SetBlendConstants");
    }

    void CommandBuffer::SetViewport(ivec2, uvec2)
    {
        NoRenderPasses("SetViewport");
    }

    void CommandBuffer::BindVertexBuffer(const Ref<Buffer>&)
    {
        NoRenderPasses("BindVertexBuffer");
    }

    void CommandBuffer::BindInstanceBuffer(const Ref<Buffer>&)
    {
        NoRenderPasses("BindInstanceBuffer");
    }

    void CommandBuffer::BindIndexBuffer(const Ref<Buffer>&, IndexType)
    {
        NoRenderPasses("BindIndexBuffer");
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
        Metal::Recording::EndEncoder(*this);
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
                const id<MTL4RenderCommandEncoder> encoder =
                    [m_Native->CommandBuffer renderCommandEncoderWithDescriptor:pass];
                if (m_Native->FirstEncoder)
                {
                    [encoder barrierAfterQueueStages:MTLStageAll
                                        beforeStages:MTLStageAll
                                   visibilityOptions:MTL4VisibilityOptionDevice];
                    m_Native->FirstEncoder = false;
                }
                [encoder endEncoding];
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
        const ivec3 sourceExtent = info.SourceExtent - info.SourceOffset;
        const ivec3 destinationExtent = info.DestinationExtent - info.DestinationOffset;
        VE_ASSERT(sourceExtent == destinationExtent,
                  "BlitImage: the Metal backend copies a blit at equal extents only ({}x{}x{} to "
                  "{}x{}x{})",
                  sourceExtent.x, sourceExtent.y, sourceExtent.z, destinationExtent.x,
                  destinationExtent.y, destinationExtent.z);
        for (u32 layer = 0; layer < info.LayerCount; ++layer)
        {
            CopyTextureRegion(*this, *info.SourceImage, info.SourceMipLevel,
                              info.SourceBaseLayer + layer, uvec3(info.SourceOffset),
                              *info.DestinationImage, info.DestinationMipLevel,
                              info.DestinationBaseLayer + layer, uvec3(info.DestinationOffset),
                              uvec3(sourceExtent));
        }
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
