#pragma once

// The Metal backend's Context::Native: the device and its one queue, the placement heaps and the
// residency set every allocation joins, the frame slots, the per-worker upload recorders, and the
// retire bins that hold what the GPU may still use. Included by Natives.h.

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "Metal.h"

#include "../../SynchronizationFrame.h"

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/TimelineSemaphore.h>

namespace Veng::Renderer
{
    struct Context::Native
    {
        /// @brief The device.
        id<MTLDevice> Device = nil;
        /// @brief The one queue graphics, transfer and readback share.
        id<MTL4CommandQueue> Queue = nil;
        /// @brief The compiler pipelines are built with.
        id<MTL4Compiler> Compiler = nil;
        /// @brief Every allocation the GPU reads: the heaps, and imported textures.
        id<MTLResidencySet> Residency = nil;
        /// @brief Whether the residency set changed since it was committed.
        bool ResidencyDirty = false;
        /// @brief Guards the residency set.
        std::mutex ResidencyMutex;

        /// @brief One placement heap and its free space.
        struct Heap
        {
            /// @brief The heap.
            id<MTLHeap> Object = nil;
            /// @brief Its free space.
            Metal::HeapBlockAllocator Space;
            /// @brief Whether it holds a single placement and is released with it.
            bool Dedicated = false;
        };

        /// @brief The shared-storage heaps.
        vector<Heap> SharedHeaps;
        /// @brief The private-storage heaps.
        vector<Heap> PrivateHeaps;
        /// @brief Guards the heaps; workers allocate upload buffers.
        std::mutex HeapMutex;

        /// @brief Whether the context has no window.
        bool Headless = true;

        /// @brief The frame slots.
        vector<SynchronizationFrame> SynchronizationFrames;
        /// @brief The slot recording or about to record.
        u32 CurrentFrameInFlight = 0;
        /// @brief The number of frame slots.
        u32 MaxFramesInFlight = 2;

        /// @brief One worker's upload recorder.
        struct TransferPool
        {
            /// @brief The worker's command buffer, with its own allocator.
            Ref<CommandBuffer> CommandBuffer;
            /// @brief The timeline value its last upload signalled.
            u64 LastSubmittedValue = 0;
        };

        /// @brief The per-worker upload recorders.
        vector<TransferPool> TransferPools;

        /// @brief Serializes queue submission and the transfer timeline's value allocation.
        std::mutex SubmitMutex;
        /// @brief The timeline uploads signal.
        Unique<TimelineSemaphore> TransferTimeline;
        /// @brief The last value allocated on the transfer timeline.
        u64 TransferTimelineValue = 0;
        /// @brief Transfer values the next frame submission waits for.
        vector<std::pair<id<MTLSharedEvent>, u64>> PendingFrameTransferWaits;

        /// @brief What a frame slot releases once its fence has been waited.
        struct RetireBin
        {
            /// @brief Metal objects the GPU may still use.
            vector<id> Objects;
            /// @brief Heap placements to return.
            vector<Metal::HeapAllocation> Allocations;
            /// @brief Run last, after the objects are released.
            vector<function<void()>> Teardowns;

            /// @brief The number of entries held.
            [[nodiscard]] usize Size() const
            {
                return Objects.size() + Allocations.size() + Teardowns.size();
            }

            /// @brief Moves every entry of @p source into this bin.
            void Append(RetireBin& source);
        };

        /// @brief Guards the bins and the transfer retire list.
        std::mutex RetireMutex;
        /// @brief One bin per frame slot.
        vector<RetireBin> RetireBins;
        /// @brief What was retired outside a recording frame.
        RetireBin PendingRetire;
        /// @brief Whether a frame is between BeginFrame and EndFrame.
        bool FrameRecording = false;
        /// @brief Whether the frame's command buffer is open.
        bool FrameCommandsOpen = false;
        /// @brief The one-shot recording in progress, or null.
        CommandBuffer* ActiveImmediateCommands = nullptr;
        /// @brief Set once teardown has drained the bins; a later retire is a lifetime bug.
        bool Disposed = false;

        /// @brief A resource kept alive until a transfer-timeline value is reached.
        struct TransferRetireEntry
        {
            /// @brief The resource.
            Ref<void> Resource;
            /// @brief The value its upload signalled.
            u64 TimelineValue = 0;
        };

        /// @brief Upload staging awaiting its transfer value.
        vector<TransferRetireEntry> TransferRetireList;

        /// @brief Push-constant ring chunks no command buffer holds.
        vector<Ref<Buffer>> FreeRingChunks;
        /// @brief Guards the free chunks.
        std::mutex RingChunkMutex;

        /// @brief Submissions whose completion feedback has not yet run.
        u64 FeedbackPending = 0;
        /// @brief Guards FeedbackPending.
        std::mutex FeedbackMutex;
        /// @brief Signalled as each feedback runs.
        std::condition_variable FeedbackDone;

        /// @brief Callbacks run as each frame slot retires.
        vector<std::pair<u64, function<void(u32)>>> FrameRetiredCallbacks;
        /// @brief The last handle issued.
        u64 NextFrameRetiredHandle = 0;

        /// @brief The event WaitIdle signals after everything committed before it.
        id<MTLSharedEvent> IdleEvent = nil;
        /// @brief The last value WaitIdle signalled.
        u64 IdleValue = 0;

        /// @brief The device readout.
        DeviceCapabilities Capabilities;
        /// @brief Whether the device decodes BC formats.
        bool BlockCompressionSupported = false;
        /// @brief Whether the device decodes ASTC formats.
        bool AstcSupported = false;
        /// @brief The queue the engine reports, one family for everything.
        QueueFamilyIndices QueueFamilies{};
        /// @brief The bindless registry.
        Unique<BindlessRegistry> Bindless;

        /// @brief Places a buffer or texture of the given size and alignment in a heap.
        /// @param shared    Whether the placement is in shared (host-visible) storage.
        /// @param size      The placement's size.
        /// @param alignment Its alignment.
        /// @return The heap and offset.
        Metal::HeapAllocation Allocate(bool shared, u64 size, u64 alignment);

        /// @brief Returns a placement to its heap; a dedicated heap is released with it.
        void Free(const Metal::HeapAllocation& allocation);

        /// @brief Adds an allocation to the residency set.
        void MakeResident(id<MTLAllocation> allocation);

        /// @brief Removes an allocation from the residency set.
        void Evict(id<MTLAllocation> allocation);

        /// @brief Commits the residency set if it changed; called before every queue commit.
        void CommitResidency();

        /// @brief Commits one command buffer and signals @p event to @p value once it completes.
        ///
        /// Waits for every pending frame transfer value first when @p waitTransfers is set.
        /// @param commandBuffer The ended command buffer.
        /// @param event         The event to signal, or nil.
        /// @param value         The value to signal.
        /// @param waitTransfers Whether to wait the pending transfer values (and clear them).
        void Commit(id<MTL4CommandBuffer> commandBuffer, id<MTLSharedEvent> event, u64 value,
                    bool waitTransfers);

        /// @brief Waits for every submission's completion feedback to have run.
        void WaitFeedback();

        /// @brief The bin a retire made now goes into.
        RetireBin& CurrentRetireBin();
        /// @brief Releases a bin's entries.
        void DrainRetireBin(RetireBin& bin);
        /// @brief Releases every bin.
        void DrainAllRetireBins();
        /// @brief Folds the pending bin into the slot just acquired.
        void AdoptPendingRetires();
        /// @brief Whether every frame slot's fence is signalled.
        [[nodiscard]] bool AreFramesComplete() const;
        /// @brief Releases what was retired outside a frame once nothing in flight can use it.
        void DrainPendingRetiresIfIdle();

        /// @brief Retires a Metal object.
        void Retire(id object);
        /// @brief Retires a heap placement.
        void Retire(const Metal::HeapAllocation& allocation);
        /// @brief Retires a teardown.
        void Retire(function<void()> teardown);
        /// @brief Keeps a resource until the transfer timeline reaches @p value.
        void RetireOnTransfer(Ref<void> resource, u64 value);
        /// @brief Drops the transfer-retired resources whose value was reached.
        void DrainTransferRetireList();

        /// @brief The frame slot a release made now waits behind.
        [[nodiscard]] u32 GetReleaseSlot() const;
        /// @brief Whether a frame slot's regions may be written now.
        [[nodiscard]] bool IsSlotWritable(u32 slot) const;

        /// @brief Takes a push-constant ring chunk from the free list or creates one.
        Ref<Buffer> AcquireRingChunk(Context& context);
        /// @brief Returns ring chunks once the work using them has completed.
        void ReleaseRingChunks(vector<Ref<Buffer>>& chunks);
    };
}
