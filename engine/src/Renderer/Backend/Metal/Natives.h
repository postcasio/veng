#pragma once

// The Metal backend's definitions of the Native structs the public Renderer headers declare.
// Objective-C++ under ARC: every Metal object here is a strong reference, released when its struct
// is destroyed or, for a resource the GPU may still use, when the retire bin it moved into drains.

#include "BarrierMapping.h"
#include "BindingModel.h"
#include "HeapAllocator.h"
#include "Metal.h"
#include "PushConstantRing.h"

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

#include <array>

namespace Veng::Renderer::Metal
{
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
        /// @brief The compute stage's argument table.
        id<MTL4ArgumentTable> ComputeTable = nil;
        /// @brief Whether recording is open.
        bool Recording = false;
        /// @brief Whether no encoder has opened since the buffer was begun, so the next one orders
        ///        itself after every earlier submission.
        bool FirstEncoder = true;

        /// @brief The bound compute pipeline.
        Ref<ComputePipeline> BoundCompute;
        /// @brief The address bound at each argument-table buffer slot, or 0.
        std::array<u64, Cook::MetalBindingSlots::BufferSlotCount> ComputeSlots{};

        /// @brief The bound layout's push block.
        Metal::PushConstantShadow Push;
        /// @brief Where the push block's copies go.
        Metal::LinearRing Ring;
        /// @brief The ring's chunks, host-mapped buffers.
        vector<Ref<Buffer>> RingChunks;
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
