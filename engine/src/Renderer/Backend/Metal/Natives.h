#pragma once

// The Metal backend's definitions of the Native structs the public Renderer headers declare.
// Objective-C++ under ARC: every Metal object here is a strong reference, released when its struct
// is destroyed or, for a resource the GPU may still use, when the retire bin it moved into drains.

#include "BarrierMapping.h"
#include "BindingModel.h"
#include "HeapAllocator.h"
#include "Metal.h"
#include "PushConstantRing.h"

#include <unordered_map>

#include "../../BarrierDecision.h"

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Fence.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Semaphore.h>
#include <Veng/Renderer/ShaderModule.h>
#include <Veng/Renderer/TimelineSemaphore.h>

namespace Veng::Renderer::Metal
{
    /// @brief The encode kernel's parameters (IndirectCount.metal), one per prepared range.
    struct IndirectCountParams
    {
        /// @brief The indirect command buffer's resource ID.
        u64 Commands = 0;
        /// @brief The first record's GPU address.
        u64 Records = 0;
        /// @brief The draw count's GPU address.
        u64 Count = 0;
        /// @brief The bound index buffer's GPU address, written by an indexed draw.
        u64 Indices = 0;
        /// @brief The bytes between records.
        u32 Stride = 0;
        /// @brief The most records drawn.
        u32 MaxDraws = 0;
        /// @brief The range's first command in the indirect command buffer.
        u32 First = 0;
        /// @brief 0 for a non-indexed draw, 1 for 16-bit indices, 2 for 32-bit; set by the draw.
        u32 IndexType = 0;
        /// @brief The draw's primitive type, or IndirectCountUndrawn until a draw consumes it.
        u32 Primitive = 0;
        /// @brief Unused.
        u32 Pad = 0;
        /// @brief The execution range's first command, written by the kernel.
        u32 RangeLocation = 0;
        /// @brief The execution range's command count, written by the kernel.
        u32 RangeLength = 0;
    };
    static_assert(sizeof(IndirectCountParams) == 64);

    /// @brief The encode kernel's threadgroup width: one thread per record.
    inline constexpr u32 IndirectCountThreadgroupSize = 64;

    /// @brief The primitive a prepared range carries until a draw consumes it.
    inline constexpr u32 IndirectCountUndrawn = 0xFFFFFFFFu;

    /// @brief What identifies a prepared range: its records' and its count's GPU addresses.
    struct IndirectCountKey
    {
        /// @brief The first record's GPU address.
        u64 Records = 0;
        /// @brief The count's GPU address.
        u64 Count = 0;
        /// @brief Equal when both addresses are.
        [[nodiscard]] bool operator==(const IndirectCountKey&) const = default;
    };

    /// @brief Hashes an IndirectCountKey.
    struct IndirectCountKeyHash
    {
        /// @brief The hash of @p key.
        [[nodiscard]] usize operator()(const IndirectCountKey& key) const
        {
            return std::hash<u64>{}(key.Records) ^ (std::hash<u64>{}(key.Count) * 31u);
        }
    };

    /// @brief Where a buffer's or texture's memory was placed.
    struct HeapAllocation
    {
        /// @brief The heap, or nil for memory the backend does not own (an imported texture).
        id<MTLHeap> Heap = nil;
        /// @brief The placement's offset in the heap.
        u64 Offset = 0;
        /// @brief The placement's size, as the device sized it.
        u64 Size = 0;
    };
}

namespace Veng::Renderer
{
    struct Buffer::Native
    {
        /// @brief The buffer.
        id<MTLBuffer> Buffer = nil;
        /// @brief Its placement.
        Metal::HeapAllocation Allocation;
        /// @brief The host mapping of a shared-storage buffer; null for a private one.
        void* Contents = nullptr;
        /// @brief Whether the host may copy into and out of the buffer directly; false for a
        ///        DeviceLocal buffer, whose transfers are staged.
        bool HostAccess = true;
        /// @brief Whether the buffer was created HostMapped, which GetMappedData requires.
        bool HostMapped = false;
    };

    struct Image::Native
    {
        /// @brief The texture.
        id<MTLTexture> Texture = nil;
        /// @brief Its placement; no heap for an imported texture.
        Metal::HeapAllocation Allocation;

        /// @brief Run after the texture is released, or empty; an imported image drops its hold on
        ///        the external texture here.
        function<void()> Teardown;

        /// @brief Per-subresource synchronization state, the render graph's barrier source.
        ///
        /// Metal images have no layouts, so the layout is tracked only because the device-free
        /// decision reads it; the stage and access decide every barrier.
        struct SubresourceState
        {
            /// @brief The subresource's layout class.
            ImageLayout Layout = ImageLayout::Undefined;
            /// @brief The stage(s) that last touched it.
            SyncStage Stage = SyncStage::TopOfPipe;
            /// @brief The access(es) that last touched it.
            SyncAccess Access = SyncAccess::None;
            /// @brief The queue family that last produced it; QueueFamilyIgnored for graphics.
            u32 ProducingFamily = QueueFamilyIgnored;
            /// @brief The transfer-timeline value an upload signalled for it, or 0.
            u64 PendingTransferValue = 0;
        };

        /// @brief The array layers tracked.
        u32 Layers = 1;
        /// @brief The mip levels tracked.
        u32 MipLevels = 1;
        /// @brief The tracked states, layer-major.
        vector<SubresourceState> States;

        /// @brief Sizes the tracking for an image.
        void InitStates(const u32 layers, const u32 mipLevels)
        {
            Layers = layers;
            MipLevels = mipLevels;
            States.assign(static_cast<usize>(layers) * mipLevels, {});
        }

        /// @brief One subresource's state.
        [[nodiscard]] SubresourceState& At(const u32 layer, const u32 mip)
        {
            return States[layer * MipLevels + mip];
        }
    };

    struct ImageView::Native
    {
        /// @brief The texture view.
        id<MTLTexture> Texture = nil;
    };

    struct Sampler::Native
    {
        /// @brief The sampler state, created for argument buffers.
        id<MTLSamplerState> Sampler = nil;
    };

    struct ShaderModule::Native
    {
        /// @brief The library holding the module's entry point.
        id<MTLLibrary> Library = nil;
    };

    struct DescriptorSetLayout::Native
    {
        /// @brief Where each binding lands in the set's argument buffer.
        Metal::SetPlacement Placement;
    };

    struct DescriptorSet::Native
    {
        /// @brief The argument buffer, one 8-byte element per argument; host-mapped.
        Ref<Buffer> Arguments;
        /// @brief Each dynamic uniform buffer's base address (buffer address plus bound offset).
        vector<u64> DynamicAddresses;

        /// @brief Writes one element.
        void WriteElement(const u32 element, const u64 value) const
        {
            auto* elements = static_cast<u64*>(Arguments->GetNative().Contents);
            elements[element] = value;
        }
    };

    struct PipelineLayout::Native
    {
        /// @brief Each set's first dynamic-uniform-buffer slot, bindless sets included.
        vector<u32> DynamicSlots;
        /// @brief The push block's size: the end of the furthest declared range.
        u32 PushBytes = 0;
    };

    struct GraphicsPipeline::Native
    {
        /// @brief The pipeline state.
        id<MTLRenderPipelineState> State = nil;
        /// @brief The depth test and write state, set beside the pipeline.
        id<MTLDepthStencilState> DepthStencil = nil;
        /// @brief The primitive every draw with the pipeline assembles.
        MTLPrimitiveType Primitive = MTLPrimitiveTypeTriangle;
        /// @brief The faces culled.
        MTLCullMode Cull = MTLCullModeNone;
        /// @brief Whether triangles rasterize as their edges.
        MTLTriangleFillMode Fill = MTLTriangleFillModeFill;
        /// @brief Whether depth is clamped to the viewport range rather than clipped.
        MTLDepthClipMode DepthClip = MTLDepthClipModeClip;
    };

    struct ComputePipeline::Native
    {
        /// @brief The pipeline state.
        id<MTLComputePipelineState> State = nil;
        /// @brief The kernel's threadgroup size, from its [numthreads].
        MTLSize ThreadsPerThreadgroup = MTLSizeMake(1, 1, 1);
    };

    struct CommandBuffer::Native
    {
        /// @brief The command buffer recorded into.
        id<MTL4CommandBuffer> CommandBuffer = nil;
        /// @brief The allocator backing it; reset when the buffer is begun again.
        id<MTL4CommandAllocator> Allocator = nil;
        /// @brief The open compute encoder, or nil.
        id<MTL4ComputeCommandEncoder> Compute = nil;
        /// @brief The open render encoder, between BeginRendering and EndRendering; or nil.
        id<MTL4RenderCommandEncoder> Render = nil;
        /// @brief The compute stage's argument table.
        id<MTL4ArgumentTable> ComputeTable = nil;
        /// @brief The vertex stage's argument table, which also carries the vertex buffers.
        id<MTL4ArgumentTable> VertexTable = nil;
        /// @brief The fragment stage's argument table.
        id<MTL4ArgumentTable> FragmentTable = nil;
        /// @brief The scaled blit's argument table, created at first use.
        id<MTL4ArgumentTable> BlitTable = nil;
        /// @brief Whether recording is open.
        bool Recording = false;
        /// @brief Whether no encoder has opened since the buffer was begun, so the next one orders
        ///        itself after every earlier submission.
        bool FirstEncoder = true;
        /// @brief The stages a recorded barrier waits on, applied by the next encoder that opens.
        Metal::MetalStage PendingAfter = Metal::MetalStage::None;
        /// @brief The stages a recorded barrier holds back that no encoder opened since has run.
        Metal::MetalStage PendingBefore = Metal::MetalStage::None;

        /// @brief The bound compute pipeline.
        Ref<ComputePipeline> BoundCompute;
        /// @brief The bound graphics pipeline, which a render pass opened after the bind applies.
        Ref<GraphicsPipeline> BoundGraphics;
        /// @brief The bound index buffer.
        Ref<Buffer> IndexBuffer;
        /// @brief The bound index buffer's index type.
        MTLIndexType IndexType = MTLIndexTypeUInt32;
        /// @brief The open render pass's attachment extent, which a viewport and scissor default to.
        uvec2 RenderExtent{0, 0};

        /// @brief The bound layout's push block.
        Metal::PushConstantShadow Push;
        /// @brief Where the push block's copies go.
        Metal::LinearRing Ring;
        /// @brief The ring's chunks, host-mapped buffers.
        vector<Ref<Buffer>> RingChunks;
        /// @brief The address of the push block's latest copy, or 0.
        u64 PushAddress = 0;
        /// @brief The push address the compute table holds.
        u64 ComputePushBound = 0;
        /// @brief The push address the render tables hold.
        u64 RenderPushBound = 0;

        /// @brief One range PrepareIndirectCount translated, which a count-driven draw executes.
        struct IndirectCountRange
        {
            /// @brief The indirect command buffer the range was encoded into.
            id<MTLIndirectCommandBuffer> Commands = nil;
            /// @brief The encode kernel's parameters, in the ring; the draw completes them.
            Metal::IndirectCountParams* Params = nullptr;
            /// @brief The parameters' GPU address.
            u64 ParamsAddress = 0;
        };
        /// @brief The ranges prepared in this recording, by their record and count addresses.
        std::unordered_map<Metal::IndirectCountKey, IndirectCountRange, Metal::IndirectCountKeyHash>
            IndirectCountRanges;
        /// @brief The indirect command buffers ranges are encoded into, kept across recordings.
        vector<id<MTLIndirectCommandBuffer>> IndirectCommandBuffers;
        /// @brief Each indirect command buffer's capacity in commands.
        vector<u32> IndirectCommandCapacities;
        /// @brief The indirect command buffer the next range is placed in.
        u32 IndirectCommandBuffer = 0;
        /// @brief The next free command in it.
        u32 IndirectCommandCursor = 0;
        /// @brief The encode kernel's argument table, created at first use.
        id<MTL4ArgumentTable> IndirectCountTable = nil;
        /// @brief Whether the open compute encoder's last work was a range's encode, so the next
        ///        needs no barrier ahead of it.
        bool IndirectCountOrdered = false;
    };

    struct Fence::Native
    {
        /// @brief The event a submission signals.
        id<MTLSharedEvent> Event = nil;
        /// @brief The value that means signalled.
        u64 Target = 0;
    };

    struct Semaphore::Native
    {
        /// @brief The event standing in for a binary semaphore.
        id<MTLSharedEvent> Event = nil;
        /// @brief The value last signalled.
        u64 Value = 0;
    };

    struct TimelineSemaphore::Native
    {
        /// @brief The event carrying the timeline.
        id<MTLSharedEvent> Event = nil;
    };
}

#include "ContextNative.h"
