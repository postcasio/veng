#pragma once

// The Metal backend's Context::Native: the device and its queues, the swap chain, the placement
// heaps and the residency set every allocation joins, the frame slots, the per-worker upload
// recorders, GPU timing, the pipeline archive, and the retire bins that hold what the GPU may still
// use. Included by Natives.h.

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

#include "Metal.h"

#include "../../SynchronizationFrame.h"

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/TimelineSemaphore.h>

namespace Veng::Renderer::Metal
{
    struct AreaClear;
}

namespace Veng::Renderer
{
    class SwapChain;

    struct Context::Native
    {
        /// @brief The device.
        id<MTLDevice> Device = nil;
        /// @brief The queue graphics, readback and presentation run on.
        id<MTL4CommandQueue> Queue = nil;
        /// @brief The queue worker uploads run on; Queue itself when the context keeps one queue.
        id<MTL4CommandQueue> TransferQueue = nil;
        /// @brief The window's swap chain; null headless.
        Unique<SwapChain> SwapChain;

        /// @brief The most GPU scopes one frame or one-shot recording times.
        static constexpr u32 MaxGpuScopes = 128;
        /// @brief The timestamps of every frame slot: per slot the frame's start and end, then a
        ///        begin and end per scope.
        id<MTL4CounterHeap> TimestampHeap = nil;
        /// @brief The timestamps of a timed one-shot recording, a begin and end per scope.
        id<MTL4CounterHeap> ImmediateTimestampHeap = nil;
        /// @brief Nanoseconds per timestamp tick.
        f64 TimestampPeriodNs = 1.0;
        /// @brief Whether each slot's timestamps were written by a frame not yet read back.
        vector<bool> TimestampWritten;
        /// @brief The scope names each slot's last frame recorded, in timestamp order.
        vector<vector<string>> ScopeNames;
        /// @brief The scope depths each slot's last frame recorded.
        vector<vector<u32>> ScopeDepths;
        /// @brief The scope names the frame being recorded has opened.
        vector<string> CurrentScopeNames;
        /// @brief The scope depths the frame being recorded has opened.
        vector<u32> CurrentScopeDepths;
        /// @brief The frame's open scopes, MaxGpuScopes standing for one left untimed.
        vector<u32> OpenScopeStack;
        /// @brief Whether a timed one-shot recording is open.
        bool ImmediateScopeRecording = false;
        /// @brief The scope names the one-shot recording has opened.
        vector<string> ImmediateScopeNames;
        /// @brief The scope depths the one-shot recording has opened.
        vector<u32> ImmediateScopeDepths;
        /// @brief The one-shot recording's open scopes.
        vector<u32> ImmediateOpenScopeStack;
        /// @brief The compiler pipelines are built with.
        id<MTL4Compiler> Compiler = nil;
        /// @brief The pipeline archive a previous run wrote, or nil.
        id<MTL4Archive> Archive = nil;
        /// @brief Records every pipeline the compiler builds, for the archive written at teardown;
        ///        nil when the context keeps no pipeline cache.
        id<MTL4PipelineDataSetSerializer> Serializer = nil;
        /// @brief Where the pipeline archive is read from and written to; empty for none.
        string ArchivePath;
        /// @brief The pipelines the archive served, which teardown builds again through the
        ///        compiler so the archive it writes keeps them.
        NSMutableArray<MTL4PipelineDescriptor*>* ArchiveServed = nil;
        /// @brief How many pipelines the archive served rather than the compiler building them.
        u64 ArchiveHits = 0;
        /// @brief How many pipelines the compiler built while the context keeps an archive.
        u64 ArchiveBuilds = 0;

        /// @brief The fence platform work encoded into a recording waits on, created at first use.
        id<MTLFence> InteropFence = nil;

        /// @brief The library of the scaled blit's shaders, compiled at first use.
        id<MTLLibrary> BlitLibrary = nil;
        /// @brief The scaled blit's linear sampler.
        id<MTLSamplerState> BlitSampler = nil;
        /// @brief The scaled blit's pipeline for each destination pixel format.
        std::unordered_map<NSUInteger, id<MTLRenderPipelineState>> BlitPipelines;
        /// @brief Guards the blit library and pipelines, and the area-clear pipelines and states.
        std::mutex BlitMutex;
        /// @brief The area clear's pipeline for each attachment layout and clear mask.
        std::unordered_map<string, id<MTLRenderPipelineState>> ClearPipelines;
        /// @brief The count-driven draw's encode kernel, built at first use from the backend's own
        ///        library.
        id<MTLComputePipelineState> IndirectCountEncoder = nil;
        /// @brief The area clear's depth state that writes depth everywhere.
        id<MTLDepthStencilState> ClearDepthWrite = nil;
        /// @brief The area clear's depth state that leaves depth alone.
        id<MTLDepthStencilState> ClearDepthKeep = nil;
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

        /// @brief A dedicated heap evicted from the residency set and not yet released.
        struct EvictedHeap
        {
            /// @brief The heap.
            id<MTLHeap> Object = nil;
            /// @brief The graphics queue's commit count when it was evicted.
            u64 QueueCommits = 0;
            /// @brief The transfer queue's commit count when it was evicted.
            u64 TransferCommits = 0;
        };

        /// @brief Evicted dedicated heaps, released once both queues have committed since.
        ///
        /// Shader validation holds each queue's view of a heap's placements until that queue's
        /// next commit after the eviction, and messages a heap released before it. Guarded by
        /// HeapMutex.
        vector<EvictedHeap> EvictedHeaps;
        /// @brief Commits made on the graphics queue.
        std::atomic<u64> QueueCommits{0};
        /// @brief Commits made on the transfer queue; the graphics queue's when they are one.
        std::atomic<u64> TransferCommits{0};

        /// @brief Releases the evicted heaps both queues have committed past.
        void ReleaseEvictedHeaps();

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
            /// @brief The autorelease pool its open recording drains at submit, or null.
            void* Pool = nullptr;
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
        /// @brief The autorelease pool the frame drains at EndFrame, or null.
        ///
        /// Metal autoreleases objects while work is recorded (its debug layer, references to every
        /// resource an encoder names), and a render thread need not drain a pool of its own; each
        /// recording drains its own, so they do not accumulate for the life of the process.
        void* FramePool = nullptr;
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

        /// @brief The event WaitIdle signals on the graphics queue after everything committed
        ///        before it.
        id<MTLSharedEvent> IdleEvent = nil;
        /// @brief The event WaitIdle signals on a dedicated transfer queue.
        id<MTLSharedEvent> TransferIdleEvent = nil;
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
        /// @param queue         The queue to commit on; the graphics queue when nil.
        void Commit(id<MTL4CommandBuffer> commandBuffer, id<MTLSharedEvent> event, u64 value,
                    bool waitTransfers, id<MTL4CommandQueue> queue = nil);

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

        /// @brief Builds a render pipeline, from the archive when it holds one.
        /// @param descriptor The pipeline.
        /// @param error      Receives the compiler's error on failure.
        /// @return The pipeline state, or nil.
        id<MTLRenderPipelineState> CreateRenderPipeline(MTL4RenderPipelineDescriptor* descriptor,
                                                        NSError** error);

        /// @brief Builds a compute pipeline, from the archive when it holds one.
        /// @param descriptor The pipeline.
        /// @param error      Receives the compiler's error on failure.
        /// @return The pipeline state, or nil.
        id<MTLComputePipelineState> CreateComputePipeline(MTL4ComputePipelineDescriptor* descriptor,
                                                          NSError** error);

        /// @brief Opens the pipeline archive at @p path, if one is there, and records what the
        ///        compiler builds from here on.
        void OpenPipelineArchive(const string& path);

        /// @brief Writes the pipeline archive, when the context keeps one and this run built a
        ///        pipeline it did not hold.
        void SavePipelineArchive();

        /// @brief The scaled blit's pipeline writing @p format, built at first use.
        id<MTLRenderPipelineState> BlitPipeline(MTLPixelFormat format);

        /// @brief The area clear's pipeline for @p clear's attachments and mask, built at first use.
        id<MTLRenderPipelineState> ClearPipeline(const Metal::AreaClear& clear);

        /// @brief The area clear's depth state, writing depth when @p writesDepth.
        id<MTLDepthStencilState> ClearDepthState(bool writesDepth);

        /// @brief The count-driven draw's encode kernel, built at first use.
        id<MTLComputePipelineState> IndirectCountPipeline();

        /// @brief Takes a push-constant ring chunk from the free list or creates one.
        Ref<Buffer> AcquireRingChunk(Context& context);
        /// @brief Returns ring chunks once the work using them has completed.
        void ReleaseRingChunks(vector<Ref<Buffer>>& chunks);
    };
}
