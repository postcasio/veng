#include <Veng/Renderer/Context.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>

#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Log.h>
#include <Veng/Renderer/AsyncReadback.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GeneratedTextureService.h>
#include <Veng/Task/TaskSystem.h>

#include <filesystem>
#include <fstream>

#include <Veng/Window.h>

#include "../../../WindowCocoa.h"
#include "../../../WindowNative.h"
#include "../../MetalFx.h"
#include "Barrier.h"
#include "Natives.h"
#include "Recording.h"
#include "SwapChain.h"
#include "TypeMapping.h"

namespace Veng
{
    extern const unsigned char g_IndirectCountLibrary[];
    extern const unsigned long g_IndirectCountLibrarySize;
}

namespace Veng::Renderer
{
    namespace
    {
        // The highest Apple GPU family the device belongs to, for the readout.
        string AppleFamilyOf(const id<MTLDevice> device)
        {
            for (i32 family = MTLGPUFamilyApple11; family >= MTLGPUFamilyApple1; --family)
            {
                if ([device supportsFamily:static_cast<MTLGPUFamily>(family)])
                {
                    return fmt::format("Apple{}", family - MTLGPUFamilyApple1 + 1);
                }
            }
            return "unknown family";
        }

        SwapChain& RequireSwapChain(const Context::Native& native)
        {
            VE_ASSERT(native.SwapChain != nullptr, "no swapchain (headless)");
            return *native.SwapChain;
        }

        // One run of timestamp scopes: the heap and first index its pairs land in, and the names,
        // depths and open stack that pair them back up at readback.
        struct ScopeRun
        {
            id<MTL4CounterHeap> Heap = nil;
            u32 FirstIndex = 0;
            vector<string>& Names;
            vector<u32>& Depths;
            vector<u32>& OpenStack;
        };

        // A timestamp lands between encoders, so an open compute encoder ends first; inside a
        // render pass none can be written, and the scope goes untimed.
        bool WriteTimestamp(const CommandBuffer& cmd, const id<MTL4CounterHeap> heap,
                            const u32 index)
        {
            const CommandBuffer::Native& native = cmd.GetNative();
            if (native.Render != nil)
            {
                return false;
            }
            Metal::Recording::EndEncoder(cmd);
            [native.CommandBuffer writeTimestampIntoHeap:heap atIndex:index];
            return true;
        }

        void OpenScope(const CommandBuffer& cmd, const ScopeRun& run, const string_view name)
        {
            // Past the budget, or where no timestamp can be written, a sentinel keeps the stack
            // balanced for the matching close.
            const auto index = static_cast<u32>(run.Names.size());
            if (index >= Context::Native::MaxGpuScopes ||
                !WriteTimestamp(cmd, run.Heap, run.FirstIndex + (index * 2)))
            {
                run.OpenStack.push_back(Context::Native::MaxGpuScopes);
                return;
            }
            run.Depths.push_back(static_cast<u32>(run.OpenStack.size()));
            run.Names.emplace_back(name);
            run.OpenStack.push_back(index);
        }

        void CloseScope(const CommandBuffer& cmd, const ScopeRun& run)
        {
            VE_ASSERT(!run.OpenStack.empty(),
                      "Context::EndGpuScope called without a matching BeginGpuScope");
            const u32 index = run.OpenStack.back();
            run.OpenStack.pop_back();
            if (index == Context::Native::MaxGpuScopes)
            {
                return;
            }
            if (!WriteTimestamp(cmd, run.Heap, run.FirstIndex + (index * 2) + 1))
            {
                // Closed inside a render pass: the scope reads as ending where it began.
                run.Names.pop_back();
                run.Depths.pop_back();
            }
        }

        // The timestamps of [first, first + count) in heap ticks.
        vector<u64> ResolveTimestamps(const id<MTL4CounterHeap> heap, const u32 first,
                                      const u32 count)
        {
            NSData* const data = [heap resolveCounterRange:NSMakeRange(first, count)];
            vector<u64> ticks(count, 0);
            if (data != nil && [data length] >= count * sizeof(MTL4TimestampHeapEntry))
            {
                const auto* entries = static_cast<const MTL4TimestampHeapEntry*>([data bytes]);
                for (u32 i = 0; i < count; ++i)
                {
                    ticks[i] = entries[i].timestamp;
                }
            }
            return ticks;
        }

        // Scope timings from resolved ticks, placed against @p base.
        vector<Context::GpuPassTiming> TimingsOf(const vector<string>& names,
                                                 const vector<u32>& depths,
                                                 const vector<u64>& ticks, const u64 base,
                                                 const f64 periodNs)
        {
            vector<Context::GpuPassTiming> timings;
            timings.reserve(names.size());
            const auto nanos = [periodNs](const u64 from, const u64 to)
            { return to > from ? static_cast<u64>(static_cast<f64>(to - from) * periodNs) : 0; };
            for (usize i = 0; i < names.size(); ++i)
            {
                const u64 begin = ticks[i * 2];
                const u64 end = ticks[(i * 2) + 1];
                timings.push_back({
                    .Name = names[i],
                    .Milliseconds = static_cast<f32>(static_cast<f64>(nanos(begin, end)) * 1e-6),
                    .BeginNanos = nanos(base, begin),
                    .EndNanos = nanos(base, end),
                    .Depth = depths[i],
                });
            }
            return timings;
        }
    }

    u64 CreatePresentationSurface(const Context& /*context*/, GLFWwindow* window)
    {
        void* const layer = AttachMetalLayer(window);
        VE_ASSERT(layer != nullptr, "the window has no Cocoa view to present through");
        return reinterpret_cast<u64>(layer);
    }

    // ---- Context::Native ---------------------------------------------------------------------

    void Context::Native::RetireBin::Append(RetireBin& source)
    {
        const auto move = [](auto& dst, auto& src)
        {
            dst.insert(dst.end(), std::make_move_iterator(src.begin()),
                       std::make_move_iterator(src.end()));
            src.clear();
        };
        move(Objects, source.Objects);
        move(Allocations, source.Allocations);
        move(Teardowns, source.Teardowns);
    }

    Metal::HeapAllocation Context::Native::Allocate(const bool shared, const u64 size,
                                                    const u64 alignment)
    {
        const std::scoped_lock lock(HeapMutex);
        vector<Heap>& heaps = shared ? SharedHeaps : PrivateHeaps;
        if (!Metal::IsDedicatedPlacement(size))
        {
            for (Heap& heap : heaps)
            {
                if (heap.Dedicated)
                {
                    continue;
                }
                if (const optional<u64> offset = heap.Space.Allocate(size, alignment))
                {
                    return {.Heap = heap.Object, .Offset = *offset, .Size = size};
                }
            }
        }

        const u64 heapBytes = Metal::HeapBytesFor(size);
        MTLHeapDescriptor* const descriptor = [[MTLHeapDescriptor alloc] init];
        descriptor.type = MTLHeapTypePlacement;
        descriptor.storageMode = shared ? MTLStorageModeShared : MTLStorageModePrivate;
        descriptor.hazardTrackingMode = MTLHazardTrackingModeUntracked;
        descriptor.size = heapBytes;
        const id<MTLHeap> object = [Device newHeapWithDescriptor:descriptor];
        VE_ASSERT(object != nil, "Context: failed to create a {}-byte {} heap", heapBytes,
                  shared ? "shared" : "private");
        [object setLabel:shared ? @"Veng Shared Heap" : @"Veng Private Heap"];
        MakeResident(object);

        Heap& heap = heaps.emplace_back(Heap{.Object = object,
                                             .Space = Metal::HeapBlockAllocator(heapBytes),
                                             .Dedicated = Metal::IsDedicatedPlacement(size)});
        const optional<u64> offset = heap.Space.Allocate(size, alignment);
        VE_ASSERT(offset.has_value(), "Context: a fresh {}-byte heap cannot hold {} bytes",
                  heapBytes, size);
        return {.Heap = heap.Object, .Offset = *offset, .Size = size};
    }

    void Context::Native::Free(const Metal::HeapAllocation& allocation)
    {
        const std::scoped_lock lock(HeapMutex);
        for (vector<Heap>* heaps : {&SharedHeaps, &PrivateHeaps})
        {
            for (auto it = heaps->begin(); it != heaps->end(); ++it)
            {
                if (it->Object != allocation.Heap)
                {
                    continue;
                }
                it->Space.Free(allocation.Offset, allocation.Size);
                if (it->Dedicated)
                {
                    Evict(it->Object);
                    EvictedHeaps.push_back({.Object = it->Object,
                                            .QueueCommits = QueueCommits.load(),
                                            .TransferCommits = TransferCommits.load()});
                    heaps->erase(it);
                }
                return;
            }
        }
        VE_ASSERT(false, "Context: freeing a placement in a heap the context does not hold");
    }

    void Context::Native::ReleaseEvictedHeaps()
    {
        const std::scoped_lock lock(HeapMutex);
        const u64 queueCommits = QueueCommits.load();
        const u64 transferCommits = TransferCommits.load();
        std::erase_if(
            EvictedHeaps, [queueCommits, transferCommits](const EvictedHeap& heap)
            { return queueCommits > heap.QueueCommits && transferCommits > heap.TransferCommits; });
    }

    void Context::Native::MakeResident(const id<MTLAllocation> allocation)
    {
        const std::scoped_lock lock(ResidencyMutex);
        [Residency addAllocation:allocation];
        ResidencyDirty = true;
    }

    void Context::Native::Evict(const id<MTLAllocation> allocation)
    {
        const std::scoped_lock lock(ResidencyMutex);
        [Residency removeAllocation:allocation];
        ResidencyDirty = true;
    }

    void Context::Native::CommitResidency()
    {
        const std::scoped_lock lock(ResidencyMutex);
        if (ResidencyDirty)
        {
            [Residency commit];
            ResidencyDirty = false;
        }
    }

    void Context::Native::Commit(const id<MTL4CommandBuffer> commandBuffer,
                                 const id<MTLSharedEvent> event, const u64 value,
                                 const bool waitTransfers, id<MTL4CommandQueue> queue)
    {
        if (queue == nil)
        {
            queue = Queue;
        }
        // A queue wait holds back every later commit, so a transfer value waited once is settled.
        if (waitTransfers)
        {
            for (const auto& [transferEvent, transferValue] : PendingFrameTransferWaits)
            {
                [queue waitForEvent:transferEvent value:transferValue];
            }
            PendingFrameTransferWaits.clear();
        }
        CommitResidency();

        MTL4CommitOptions* const options = [[MTL4CommitOptions alloc] init];
        {
            const std::scoped_lock lock(FeedbackMutex);
            ++FeedbackPending;
        }
        Native* const self = this;
        [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
          NSError* const error = [feedback error];
          if (error != nil)
          {
#ifdef VE_ENABLE_VALIDATION_LAYERS
              Log::Error("Metal validation: {}", Metal::Describe(error));
#else
              Log::Error("Metal command buffer failed: {}", Metal::Describe(error));
#endif
          }
          {
              const std::scoped_lock lock(self->FeedbackMutex);
              --self->FeedbackPending;
          }
          self->FeedbackDone.notify_all();
        }];

        const id<MTL4CommandBuffer> buffers[] = {commandBuffer};
        [queue commit:buffers count:1 options:options];
        if (queue == Queue)
        {
            ++QueueCommits;
        }
        if (queue == TransferQueue)
        {
            ++TransferCommits;
        }
        if (event != nil)
        {
            [queue signalEvent:event value:value];
        }
    }

    void Context::Native::WaitFeedback()
    {
        std::unique_lock lock(FeedbackMutex);
        FeedbackDone.wait(lock, [this] { return FeedbackPending == 0; });
    }

    Context::Native::RetireBin& Context::Native::CurrentRetireBin()
    {
        VE_ASSERT(!Disposed,
                  "Context::Retire after teardown — a resource outlived the rendering context");
        // Between EndFrame and the next BeginFrame the current slot is the one about to be waited
        // and drained, while what is retired now was last used by the frame just submitted; it
        // waits in the pending bin until BeginFrame folds it into the acquired slot's.
        if (!FrameRecording)
        {
            return PendingRetire;
        }
        return RetireBins[CurrentFrameInFlight];
    }

    void Context::Native::Retire(id object)
    {
        if (object == nil)
        {
            return;
        }
        const std::scoped_lock lock(RetireMutex);
        CurrentRetireBin().Objects.push_back(object);
    }

    void Context::Native::Retire(const Metal::HeapAllocation& allocation)
    {
        const std::scoped_lock lock(RetireMutex);
        CurrentRetireBin().Allocations.push_back(allocation);
    }

    void Context::Native::Retire(function<void()> teardown)
    {
        const std::scoped_lock lock(RetireMutex);
        CurrentRetireBin().Teardowns.push_back(std::move(teardown));
    }

    void Context::Native::RetireOnTransfer(Ref<void> resource, const u64 value)
    {
        const std::scoped_lock lock(RetireMutex);
        TransferRetireList.push_back({.Resource = std::move(resource), .TimelineValue = value});
    }

    void Context::Native::DrainTransferRetireList()
    {
        // Released outside the lock: a dropped resource retires itself, which takes it again.
        vector<TransferRetireEntry> reached;
        {
            const std::scoped_lock lock(RetireMutex);
            const u64 value = TransferTimeline->GetValue();
            const auto split =
                std::ranges::partition(TransferRetireList, [value](const TransferRetireEntry& entry)
                                       { return entry.TimelineValue > value; });
            reached.assign(std::make_move_iterator(split.begin()),
                           std::make_move_iterator(split.end()));
            TransferRetireList.erase(split.begin(), split.end());
        }
    }

    void Context::Native::DrainRetireBin(RetireBin& bin)
    {
        // Taken out under the lock and released outside it: a teardown may drop a resource, which
        // retires itself.
        RetireBin drained;
        {
            const std::scoped_lock lock(RetireMutex);
            drained = std::exchange(bin, {});
        }
        // Objects first, then the memory they were placed in, then the teardowns.
        drained.Objects.clear();
        for (const Metal::HeapAllocation& allocation : drained.Allocations)
        {
            Free(allocation);
        }
        for (const auto& teardown : drained.Teardowns)
        {
            teardown();
        }
        ReleaseEvictedHeaps();
    }

    void Context::Native::DrainAllRetireBins()
    {
        for (auto& bin : RetireBins)
        {
            DrainRetireBin(bin);
        }
        DrainRetireBin(PendingRetire);
    }

    void Context::Native::AdoptPendingRetires()
    {
        const std::scoped_lock lock(RetireMutex);
        RetireBins[CurrentFrameInFlight].Append(PendingRetire);
        FrameRecording = true;
    }

    bool Context::Native::AreFramesComplete() const
    {
        return std::ranges::all_of(SynchronizationFrames, [](const SynchronizationFrame& frame)
                                   { return frame.GetInFlightFence().IsSignaled(); });
    }

    void Context::Native::DrainPendingRetiresIfIdle()
    {
        // Released bindless slots first, so what their reclaim lets go of retires into the bin
        // detached below and is released by this same drain.
        if (Bindless && AreFramesComplete())
        {
            Bindless->ReclaimAllReleased();
        }
        DrainTransferRetireList();

        RetireBin detached;
        {
            const std::scoped_lock lock(RetireMutex);
            if (PendingRetire.Size() == 0)
            {
                return;
            }
            detached = std::exchange(PendingRetire, {});
        }

        u64 transferSubmitted = 0;
        {
            const std::scoped_lock lock(SubmitMutex);
            transferSubmitted = TransferTimelineValue;
        }

        // Neither check blocks; while either fails the entries go back to pending, for the next
        // BeginFrame to adopt or a later immediate submit to retry.
        const bool framesComplete = AreFramesComplete();
        const bool transferComplete = TransferTimeline->GetValue() >= transferSubmitted;
        if (framesComplete && transferComplete)
        {
            DrainRetireBin(detached);
            return;
        }
        const std::scoped_lock lock(RetireMutex);
        PendingRetire.Append(detached);
    }

    u32 Context::Native::GetReleaseSlot() const
    {
        if (FrameRecording)
        {
            return CurrentFrameInFlight;
        }
        return (CurrentFrameInFlight + MaxFramesInFlight - 1) % MaxFramesInFlight;
    }

    bool Context::Native::IsSlotWritable(const u32 slot) const
    {
        if (FrameRecording && slot == CurrentFrameInFlight)
        {
            return true;
        }
        return SynchronizationFrames[slot].GetInFlightFence().IsSignaled();
    }

    Ref<Buffer> Context::Native::AcquireRingChunk(Context& context)
    {
        {
            const std::scoped_lock lock(RingChunkMutex);
            if (!FreeRingChunks.empty())
            {
                Ref<Buffer> chunk = std::move(FreeRingChunks.back());
                FreeRingChunks.pop_back();
                return chunk;
            }
        }
        return Buffer::Create(context, {
                                           .Name = "Push Constant Ring",
                                           .Size = Metal::PushConstantChunkBytes,
                                           .Usage = BufferUsage::Uniform,
                                           .HostMapped = true,
                                       });
    }

    void Context::Native::ReleaseRingChunks(vector<Ref<Buffer>>& chunks)
    {
        if (chunks.empty())
        {
            return;
        }
        // Returned once the work that read them has completed.
        // A lambda converts to an Objective-C block as readily as to a function, so it is named.
        Retire(function<void()>(
            [this, released = std::move(chunks)]() mutable
            {
                const std::scoped_lock lock(RingChunkMutex);
                for (Ref<Buffer>& chunk : released)
                {
                    FreeRingChunks.push_back(std::move(chunk));
                }
            }));
        chunks.clear();
    }

    id<MTLRenderPipelineState>
    Context::Native::CreateRenderPipeline(MTL4RenderPipelineDescriptor* const descriptor,
                                          NSError** const error)
    {
        if (CountArchiveHits && Archive != nil &&
            [Archive newRenderPipelineStateWithDescriptor:descriptor error:nil] != nil)
        {
            ++ArchiveHits;
        }
        // Created through the compiler even when the archive holds it, so the serializer records
        // it and the archive written at teardown keeps it.
        ArchiveBuilds += Serializer != nil ? 1 : 0;
        return [Compiler newRenderPipelineStateWithDescriptor:descriptor
                                          compilerTaskOptions:ArchiveLookup
                                                        error:error];
    }

    id<MTLComputePipelineState>
    Context::Native::CreateComputePipeline(MTL4ComputePipelineDescriptor* const descriptor,
                                           NSError** const error)
    {
        if (CountArchiveHits && Archive != nil &&
            [Archive newComputePipelineStateWithDescriptor:descriptor error:nil] != nil)
        {
            ++ArchiveHits;
        }
        ArchiveBuilds += Serializer != nil ? 1 : 0;
        return [Compiler newComputePipelineStateWithDescriptor:descriptor
                                           compilerTaskOptions:ArchiveLookup
                                                         error:error];
    }

    id<MTLComputePipelineState> Context::Native::IndirectCountPipeline()
    {
        const std::scoped_lock lock(BlitMutex);
        if (IndirectCountEncoder != nil)
        {
            return IndirectCountEncoder;
        }
        const dispatch_data_t bytes =
            dispatch_data_create(g_IndirectCountLibrary, g_IndirectCountLibrarySize, nullptr,
                                 DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError* error = nil;
        const id<MTLLibrary> library = [Device newLibraryWithData:bytes error:&error];
        VE_ASSERT(library != nil, "Context: the indirect-count library failed to load: {}",
                  Metal::Describe(error));
        MTL4LibraryFunctionDescriptor* const function =
            [[MTL4LibraryFunctionDescriptor alloc] init];
        function.library = library;
        function.name = @"EncodeIndirectCount";
        MTL4ComputePipelineDescriptor* const descriptor =
            [[MTL4ComputePipelineDescriptor alloc] init];
        descriptor.computeFunctionDescriptor = function;
        descriptor.requiredThreadsPerThreadgroup =
            MTLSizeMake(Metal::IndirectCountThreadgroupSize, 1, 1);
        descriptor.label = @"Indirect Count Encoder";
        IndirectCountEncoder = CreateComputePipeline(descriptor, &error);
        VE_ASSERT(IndirectCountEncoder != nil,
                  "Context: the indirect-count encoder failed to build: {}",
                  Metal::Describe(error));
        return IndirectCountEncoder;
    }

    void Context::Native::OpenPipelineArchive(const path& cachePath)
    {
        // Metal 4's capture layer refuses a process that has opened an archive, so a run that can
        // be captured keeps none, and builds every pipeline.
        MTLCaptureManager* const capture = [MTLCaptureManager sharedCaptureManager];
        if ([capture supportsDestination:MTLCaptureDestinationDeveloperTools] ||
            [capture supportsDestination:MTLCaptureDestinationGPUTraceDocument])
        {
            Log::Info(
                "Pipeline archive off: GPU capture is available, and Metal 4 capture does not "
                "support archives");
            return;
        }
        // Metal's validation layers fault compiling against a lookup archive, and shader
        // validation instruments every pipeline, which no archive holds.
        if (Metal::IsEnvironmentSwitchOn("MTL_DEBUG_LAYER") ||
            Metal::IsEnvironmentSwitchOn("MTL_SHADER_VALIDATION"))
        {
            Log::Info("Pipeline archive off: Metal's validation layer is on");
            return;
        }
        path archive = cachePath;
        archive.replace_filename(cachePath.stem().string() + ".metal" +
                                 cachePath.extension().string());
        ArchivePath = archive.string();
        MTL4PipelineDataSetSerializerDescriptor* const serializer =
            [[MTL4PipelineDataSetSerializerDescriptor alloc] init];
        serializer.configuration = MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
        Serializer = [Device newPipelineDataSetSerializerWithDescriptor:serializer];

        std::error_code exists;
        if (!std::filesystem::exists(archive, exists))
        {
            return;
        }
        // Metal does not refuse a file that is not an archive: it faults reading one. So the
        // header is checked first. An archive is a Mach-O image, thin or universal.
        constexpr usize MachOHeaderSize = 32;
        std::array<u8, 4> head{};
        std::error_code sized;
        const std::uintmax_t size = std::filesystem::file_size(archive, sized);
        std::ifstream file(archive, std::ios::binary);
        if (sized || !file || !file.read(reinterpret_cast<char*>(head.data()), head.size()))
        {
            Log::Warn("Pipeline archive '{}' could not be read; building every pipeline",
                      ArchivePath);
            return;
        }
        const u32 bigEndian =
            (u32{head[0]} << 24) | (u32{head[1]} << 16) | (u32{head[2]} << 8) | u32{head[3]};
        // The thin 64-bit magic as it lies on disk, then the universal forms Metal writes.
        const bool machO = bigEndian == 0xCFFAEDFEu || bigEndian == 0xCAFEBABEu ||
                           bigEndian == 0xCAFEBABFu || bigEndian == 0xCBFEBABEu;
        if (size < MachOHeaderSize || !machO)
        {
            Log::Warn("Pipeline archive '{}' is not a Metal archive; building every pipeline",
                      ArchivePath);
            return;
        }
        // A stale or truncated archive is refused here, and the run starts cold.
        NSError* error = nil;
        Archive = [Device newArchiveWithURL:[NSURL fileURLWithPath:Metal::ToNSString(ArchivePath)]
                                      error:&error];
        if (Archive == nil)
        {
            Log::Warn("Pipeline archive '{}' could not be read; building every pipeline: {}",
                      ArchivePath, Metal::Describe(error));
            return;
        }
        ArchiveLookup = [[MTL4CompilerTaskOptions alloc] init];
        ArchiveLookup.lookupArchives = @[ Archive ];
    }

    void Context::Native::SavePipelineArchive()
    {
        // Every pipeline went through the compiler, so the serializer already holds the run's set.
        ArchiveLookup = nil;
        Archive = nil;
        if (Serializer == nil || ArchiveBuilds == 0)
        {
            return;
        }

        // Written beside the archive and moved over it, so a failed write leaves the last one.
        const string staging = ArchivePath + ".incoming";
        NSError* error = nil;
        if (![Serializer
                serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:Metal::ToNSString(staging)]
                                          error:&error])
        {
            Log::Warn("Pipeline archive '{}' could not be written: {}", ArchivePath,
                      Metal::Describe(error));
            return;
        }
        std::error_code renamed;
        std::filesystem::rename(staging, ArchivePath, renamed);
        if (renamed)
        {
            Log::Warn("Pipeline archive '{}' could not be replaced: {}", ArchivePath,
                      renamed.message());
        }
    }

    // ---- Context -----------------------------------------------------------------------------

    Context::Context() : m_Native(CreateUnique<Native>()) {}

    Context::~Context()
    {
        // A context that never completed Initialize has no device and nothing to tear down.
        if (m_Native->Device == nil)
        {
            return;
        }
        WaitIdle();
        ReleaseFrameResources();
        DestroyDevice();
    }

    Context::Native& Context::GetNative() const
    {
        return *m_Native;
    }

    void Context::Initialize(const ContextInfo& info, Window* window)
    {
        Native& native = *m_Native;
        native.Headless = window == nullptr;
        m_Window = window;
        Log::Info(native.Headless ? "Initializing Metal context (headless)"
                                  : "Initializing Metal context");

        native.Device = MTLCreateSystemDefaultDevice();
        VE_ASSERT(native.Device != nil, "Context: this system has no Metal device");
        VE_ASSERT([native.Device supportsFamily:MTLGPUFamilyMetal4],
                  "Context: '{}' does not support the Metal 4 GPU family the Metal backend needs "
                  "(Apple silicon on macOS 26 or later)",
                  Metal::ToString([native.Device name]));

        native.Queue = [native.Device newMTL4CommandQueue];
        VE_ASSERT(native.Queue != nil, "Context: failed to create the Metal 4 command queue");
        native.TransferQueue = native.Queue;
        if (info.DedicatedTransferQueue)
        {
            native.TransferQueue = [native.Device newMTL4CommandQueue];
            VE_ASSERT(native.TransferQueue != nil,
                      "Context: failed to create the Metal 4 transfer queue");
        }

        NSError* error = nil;
        MTLResidencySetDescriptor* const residency = [[MTLResidencySetDescriptor alloc] init];
        residency.label = @"Veng Residency";
        residency.initialCapacity = 64;
        native.Residency = [native.Device newResidencySetWithDescriptor:residency error:&error];
        VE_ASSERT(native.Residency != nil, "Context: failed to create the residency set: {}",
                  Metal::Describe(error));
        [native.Queue addResidencySet:native.Residency];
        if (native.TransferQueue != native.Queue)
        {
            [native.TransferQueue addResidencySet:native.Residency];
        }

        if (info.PipelineCachePath.has_value())
        {
            native.OpenPipelineArchive(*info.PipelineCachePath);
        }
        MTL4CompilerDescriptor* const compiler = [[MTL4CompilerDescriptor alloc] init];
        compiler.label = @"Veng Compiler";
        compiler.pipelineDataSetSerializer = native.Serializer;
        native.Compiler = [native.Device newCompilerWithDescriptor:compiler error:&error];
        VE_ASSERT(native.Compiler != nil, "Context: failed to create the Metal compiler: {}",
                  Metal::Describe(error));

        native.IdleEvent = [native.Device newSharedEvent];
        native.TransferIdleEvent = [native.Device newSharedEvent];

        // Graphics and presentation share a queue; transfer is a family of its own when it has a
        // queue of its own.
        const u32 transferFamily = native.TransferQueue != native.Queue ? 1u : 0u;
        native.QueueFamilies = {
            .GraphicsFamily = 0u, .PresentFamily = 0u, .TransferFamily = transferFamily};
        Log::Info("Queues: graphics and present on one{}",
                  transferFamily != 0 ? ", transfer on its own" : ", transfer collapsed onto it");
        // The submission mode is a MoltenVK setting; Metal commits asynchronously.
        m_SubmitMode = {.Mode = QueueSubmitMode::Asynchronous,
                        .Source = QueueSubmitModeSource::DriverDefault,
                        .ApplySetting = false};

        DeviceCapabilities& capabilities = native.Capabilities;
        capabilities.DeviceName = Metal::ToString([native.Device name]);
        capabilities.DriverName = "Metal";
        capabilities.DriverInfo = AppleFamilyOf(native.Device);
        capabilities.ApiVersion = 4u << 22u;
        // The constant and device address spaces read a 16-byte vector at a 16-byte offset.
        capabilities.UniformBufferOffsetAlignment = 16;
        capabilities.StorageBufferOffsetAlignment = 16;
        // Every Apple GPU runs 32-wide SIMD-groups, with the SIMD-group and quad functions in
        // every stage.
        capabilities.SubgroupSize = 32;
        capabilities.SubgroupStages =
            ShaderStage::Vertex | ShaderStage::Fragment | ShaderStage::Compute;
        capabilities.SubgroupOperations =
            SubgroupOperation::Basic | SubgroupOperation::Vote | SubgroupOperation::Arithmetic |
            SubgroupOperation::Ballot | SubgroupOperation::Shuffle |
            SubgroupOperation::ShuffleRelative | SubgroupOperation::Quad;
        capabilities.ShaderFloat16 = true;
        capabilities.ShaderInt16 = true;
        capabilities.StorageBuffer16BitAccess = true;
        capabilities.B10G11R11UfloatStorage = true;
        capabilities.RGBA16SfloatStorage = true;
        capabilities.MeshShading = [native.Device supportsFamily:MTLGPUFamilyMetal3];
        capabilities.RayTracing = [native.Device supportsRaytracing];
        // A count-driven draw executes an indirect command buffer a compute dispatch encodes. Metal's
        // shader validation executes no draw such a buffer holds, so a process running under it
        // takes the fixed-maximum path. A tile-based Apple GPU keeps a memoryless attachment in tile
        // memory alone.
        capabilities.IndirectDrawCount = !Metal::IsEnvironmentSwitchOn("MTL_SHADER_VALIDATION");
        capabilities.MemorylessAttachments = true;
        native.BlockCompressionSupported = [native.Device supportsBCTextureCompression];
        native.AstcSupported = [native.Device supportsFamily:MTLGPUFamilyApple2];

        const MetalFxSupport metalFx = QueryMetalFxSupport(*this);
        capabilities.MetalFxTemporal = metalFx.Temporal;
        capabilities.MetalFxSpatial = metalFx.Spatial;
        Log::Info("{}", DescribeDeviceCapabilities(capabilities));

        if (!native.Headless)
        {
            window->CreateSurface(*this);
            auto* const layer = (__bridge CAMetalLayer*)reinterpret_cast<void*>(
                window->GetNative().PresentationSurface);
            // Three drawables, one more than frames in flight, so the frame being recorded never
            // waits on a drawable the display still holds.
            native.SwapChain = CreateUnique<SwapChain>(
                *this, layer, uvec2{window->GetWidth(), window->GetHeight()}, 3u,
                info.RequestedDisplayMode);
            [native.Queue addResidencySet:[layer residencySet]];
            m_RenderExtent = native.SwapChain->GetExtent();
            Log::Info("Swap chain: {} drawables ({}x{}), {}", native.SwapChain->GetImageCount(),
                      m_RenderExtent.x, m_RenderExtent.y,
                      FormatName(native.SwapChain->GetSurface().EngineFormat));
        }
        else
        {
            m_RenderExtent = info.HeadlessExtent;
        }
        m_OutputFormat = info.OutputFormat;
        m_DepthFormat = info.DepthFormat;

        // GPU timing: per frame slot a start and an end stamp and a begin and end per scope, and a
        // heap of its own for one-shot recordings.
        MTL4CounterHeapDescriptor* const heap = [[MTL4CounterHeapDescriptor alloc] init];
        heap.type = MTL4CounterHeapTypeTimestamp;
        heap.count = native.MaxFramesInFlight * (2 + (2 * Native::MaxGpuScopes));
        native.TimestampHeap = [native.Device newCounterHeapWithDescriptor:heap error:&error];
        heap.count = 2 * Native::MaxGpuScopes;
        native.ImmediateTimestampHeap = [native.Device newCounterHeapWithDescriptor:heap
                                                                              error:&error];
        m_GpuTimingSupported = native.TimestampHeap != nil && native.ImmediateTimestampHeap != nil;
        if (m_GpuTimingSupported)
        {
            native.TimestampPeriodNs =
                1e9 / static_cast<f64>([native.Device queryTimestampFrequency]);
            native.TimestampWritten.assign(native.MaxFramesInFlight, false);
            native.ScopeNames.resize(native.MaxFramesInFlight);
            native.ScopeDepths.resize(native.MaxFramesInFlight);
        }
        else
        {
            Log::Warn("GPU timing unavailable: no timestamp counter heap: {}",
                      Metal::Describe(error));
        }

        native.SynchronizationFrames.reserve(native.MaxFramesInFlight);
        for (u32 i = 0; i < native.MaxFramesInFlight; ++i)
        {
            native.SynchronizationFrames.emplace_back(*this);
        }
        native.RetireBins.resize(native.MaxFramesInFlight);
        native.TransferTimeline = TimelineSemaphore::Create(*this);

        native.Bindless = CreateUnique<BindlessRegistry>(*this);
        m_GeneratedTextures = CreateUnique<GeneratedTextureService>(*this);
        m_AsyncReadback = CreateUnique<AsyncReadback>(*this);

        // The registry's setup work runs now, so the first frame or one-shot submit inherits none.
        if (!m_PendingSetupCommands.empty())
        {
            ImmediateCommands([](CommandBuffer&) {});
        }
    }

    void Context::ReleaseFrameResources()
    {
        m_PendingBindlessAcquires.clear();
        m_PendingSetupCommands.clear();
        m_GeneratedTextures.reset();
        m_AsyncReadback.reset();

        m_Native->DrainAllRetireBins();
        m_Native->TransferTimeline->Wait(m_Native->TransferTimelineValue);
        m_Native->DrainTransferRetireList();

        m_Native->SynchronizationFrames.clear();
        m_Native->SwapChain.reset();
    }

    void Context::DestroyDevice()
    {
        Log::Info("Tearing down rendering context...");
        Native& native = *m_Native;

        native.Bindless.reset();
        for (auto& pool : native.TransferPools)
        {
            pool.CommandBuffer.reset();
        }
        native.TransferPools.clear();

        // The command buffers' ring chunks return to the free list through the bins, and the free
        // list's buffers retire into them in turn, so the bins drain on either side of it.
        native.DrainAllRetireBins();
        {
            const std::scoped_lock lock(native.RingChunkMutex);
            native.FreeRingChunks.clear();
        }
        native.DrainAllRetireBins();
        native.TransferTimeline->Wait(native.TransferTimelineValue);
        native.DrainTransferRetireList();
        native.DrainAllRetireBins();
        native.WaitFeedback();
        if (native.GpuCaptureFramesLeft > 0)
        {
            [[MTLCaptureManager sharedCaptureManager] stopCapture];
            native.GpuCaptureFramesLeft = 0;
        }
        native.Disposed = true;

        native.TransferTimeline.reset();
        native.SharedHeaps.clear();
        native.PrivateHeaps.clear();
        native.SavePipelineArchive();
        native.Serializer = nil;
        native.BlitPipelines.clear();
        native.BlitLibrary = nil;
        native.BlitSampler = nil;
        native.TimestampHeap = nil;
        native.ImmediateTimestampHeap = nil;
        native.Residency = nil;
        native.Compiler = nil;
        native.Queue = nil;
        native.TransferQueue = nil;
        native.IdleEvent = nil;
        native.TransferIdleEvent = nil;
        native.Device = nil;
        m_Window = nullptr;
    }

    void Context::InitializeTransferPools(TaskSystem& taskSystem)
    {
        const u32 workerCount = taskSystem.GetWorkerCount();
        m_Native->TransferPools.resize(workerCount);
        // Each worker's command buffer carries an allocator of its own, which one thread records
        // through at a time.
        for (u32 workerIndex = 0; workerIndex < workerCount; ++workerIndex)
        {
            m_Native->TransferPools[workerIndex].CommandBuffer = CommandBuffer::Create(*this);
        }
        m_GeneratedTextures->SetTaskSystem(&taskSystem);
        Log::Info("Created {0} per-worker transfer command buffer(s)", workerCount);
    }

    CommandBuffer& Context::GetTransferCommandBuffer(u32 workerIndex)
    {
        VE_ASSERT(workerIndex < m_Native->TransferPools.size(),
                  "GetTransferCommandBuffer: worker index {} out of range ({} transfer pools) — "
                  "InitializeTransferPools must run first",
                  workerIndex, m_Native->TransferPools.size());
        return *m_Native->TransferPools[workerIndex].CommandBuffer;
    }

    CommandBuffer& Context::BeginTransferRecording(u32 workerIndex)
    {
        VE_ASSERT(workerIndex < m_Native->TransferPools.size(),
                  "BeginTransferRecording: worker index {} out of range ({} transfer pools)",
                  workerIndex, m_Native->TransferPools.size());
        auto& pool = m_Native->TransferPools[workerIndex];
        // The worker's one command buffer is reused once its last upload has completed.
        if (pool.LastSubmittedValue != 0)
        {
            m_Native->TransferTimeline->Wait(pool.LastSubmittedValue);
        }
        VE_ASSERT(pool.Pool == nullptr,
                  "BeginTransferRecording: worker {} began a recording it never submitted",
                  workerIndex);
        pool.Pool = objc_autoreleasePoolPush();
        pool.CommandBuffer->Begin(CommandBufferUsage::OneTimeSubmit);
        return *pool.CommandBuffer;
    }

    u64 Context::SubmitTransfer(u32 workerIndex, const TimelineSemaphore& timeline)
    {
        VE_ASSERT(workerIndex < m_Native->TransferPools.size(),
                  "SubmitTransfer: worker index {} out of range ({} transfer pools)", workerIndex,
                  m_Native->TransferPools.size());
        auto& pool = m_Native->TransferPools[workerIndex];
        pool.CommandBuffer->End();

        // The value is allocated and signalled under one lock, so a timeline's values signal in
        // increasing order.
        const std::scoped_lock lock(m_Native->SubmitMutex);
        const u64 value = ++m_Native->TransferTimelineValue;
        m_Native->Commit(pool.CommandBuffer->GetNative().CommandBuffer, timeline.GetNative().Event,
                         value, false, m_Native->TransferQueue);
        pool.LastSubmittedValue = value;
        objc_autoreleasePoolPop(pool.Pool);
        pool.Pool = nullptr;
        return value;
    }

    TimelineSemaphore& Context::GetTransferTimeline() const
    {
        return *m_Native->TransferTimeline;
    }

    bool Context::IsHeadless() const
    {
        return m_Native->Headless;
    }

    const QueueFamilyIndices& Context::GetQueueFamilies() const
    {
        return m_Native->QueueFamilies;
    }

    bool Context::IsGpuDrivenCullingSupported() const
    {
        // A multi-draw indirect is one indirect draw per record, each carrying its base instance.
        return true;
    }

    bool Context::IsBlockCompressionSupported() const
    {
        return m_Native->BlockCompressionSupported;
    }

    bool Context::IsAstcSupported() const
    {
        return m_Native->AstcSupported;
    }

    bool Context::IsDepthClampSupported() const
    {
        return true;
    }

    bool Context::IsExtendedStorageImageFormatsSupported() const
    {
        return true;
    }

    const DeviceCapabilities& Context::GetDeviceCapabilities() const
    {
        return m_Native->Capabilities;
    }

    Context::GpuMemoryUsage Context::GetGpuMemoryUsage() const
    {
        return {.UsedBytes = [m_Native->Device currentAllocatedSize],
                .BudgetBytes = [m_Native->Device recommendedMaxWorkingSetSize]};
    }

    void Context::BeginGpuScope(CommandBuffer& cmd, const string_view name)
    {
        Native& native = *m_Native;
#if defined(VE_DEBUG) && VE_DEBUG
        // A named region in a capture: a debug group on the open render pass, or between encoders.
        const CommandBuffer::Native& recording = cmd.GetNative();
        if (recording.Recording)
        {
            if (recording.Render != nil)
            {
                [recording.Render pushDebugGroup:Metal::ToNSString(name)];
            }
            else
            {
                Metal::Recording::EndEncoder(cmd);
                [recording.CommandBuffer pushDebugGroup:Metal::ToNSString(name)];
            }
        }
#endif
        if (!m_GpuTimingSupported)
        {
            return;
        }
        if (native.ImmediateScopeRecording)
        {
            OpenScope(cmd,
                      {.Heap = native.ImmediateTimestampHeap,
                       .Names = native.ImmediateScopeNames,
                       .Depths = native.ImmediateScopeDepths,
                       .OpenStack = native.ImmediateOpenScopeStack},
                      name);
            return;
        }
        if (!m_GpuScopeRecording)
        {
            return;
        }
        OpenScope(
            cmd,
            {.Heap = native.TimestampHeap,
             .FirstIndex = (native.CurrentFrameInFlight * (2 + (2 * Native::MaxGpuScopes))) + 2,
             .Names = native.CurrentScopeNames,
             .Depths = native.CurrentScopeDepths,
             .OpenStack = native.OpenScopeStack},
            name);
    }

    void Context::EndGpuScope(CommandBuffer& cmd)
    {
        Native& native = *m_Native;
        if (m_GpuTimingSupported && native.ImmediateScopeRecording)
        {
            CloseScope(cmd, {.Heap = native.ImmediateTimestampHeap,
                             .Names = native.ImmediateScopeNames,
                             .Depths = native.ImmediateScopeDepths,
                             .OpenStack = native.ImmediateOpenScopeStack});
        }
        else if (m_GpuTimingSupported && m_GpuScopeRecording)
        {
            CloseScope(
                cmd,
                {.Heap = native.TimestampHeap,
                 .FirstIndex = (native.CurrentFrameInFlight * (2 + (2 * Native::MaxGpuScopes))) + 2,
                 .Names = native.CurrentScopeNames,
                 .Depths = native.CurrentScopeDepths,
                 .OpenStack = native.OpenScopeStack});
        }
#if defined(VE_DEBUG) && VE_DEBUG
        const CommandBuffer::Native& recording = cmd.GetNative();
        if (recording.Recording)
        {
            if (recording.Render != nil)
            {
                [recording.Render popDebugGroup];
            }
            else
            {
                Metal::Recording::EndEncoder(cmd);
                [recording.CommandBuffer popDebugGroup];
            }
        }
#endif
    }

    bool Context::IsFormatLinearFilterSupported(const Format format) const
    {
        switch (format)
        {
        case Format::RGBA16Uint:
        case Format::R32Uint:
        case Format::S8Uint:
            return false;
        case Format::R32Sfloat:
        case Format::RG32Sfloat:
        case Format::RGBA32Sfloat:
            return [m_Native->Device supports32BitFloatFiltering];
        default:
            return Metal::ToMetal(format).has_value();
        }
    }

    bool Context::IsFormatColorAttachmentTransferSrcSupported(const Format format) const
    {
        return Metal::ToMetal(format).has_value() && !Metal::IsCompressed(format) &&
               !Metal::IsDepthStencil(format) && format != Format::Undefined;
    }

    bool Context::IsFormatStorageImageSupported(const Format format) const
    {
        // Compressed, depth and sRGB-encoded textures take no shader writes.
        switch (format)
        {
        case Format::RGBA8Srgb:
        case Format::BGRA8Srgb:
        case Format::BC7Srgb:
        case Format::ASTC4x4Srgb:
        case Format::Undefined:
            return false;
        default:
            return Metal::ToMetal(format).has_value() && !Metal::IsCompressed(format) &&
                   !Metal::IsDepthStencil(format);
        }
    }

    bool Context::IsRecordingFrame() const
    {
        return m_Native->FrameCommandsOpen && m_Native->ActiveImmediateCommands == nullptr;
    }

    SynchronizationFrame& Context::GetCurrentFrame()
    {
        return m_Native->SynchronizationFrames[m_Native->CurrentFrameInFlight];
    }

    CommandBuffer& Context::GetCurrentCommandBuffer()
    {
        return *GetCurrentFrame().GetCommandBuffer();
    }

    SynchronizationFrame& Context::AcquireNextFrame()
    {
        {
            VE_PROFILE_SCOPE("Render/FenceWait");
            m_Native->SynchronizationFrames[m_Native->CurrentFrameInFlight]
                .GetInFlightFence()
                .Wait();
        }
        {
            VE_PROFILE_SCOPE("Render/RetireDrain");
            m_Native->DrainRetireBin(m_Native->RetireBins[m_Native->CurrentFrameInFlight]);
            m_Native->DrainTransferRetireList();
        }
        m_Native->Bindless->OnFrameAcquired(m_Native->CurrentFrameInFlight);
        return m_Native->SynchronizationFrames[m_Native->CurrentFrameInFlight];
    }

    CommandBuffer& Context::BeginFrame()
    {
        VE_ASSERT(m_Native->FramePool == nullptr, "BeginFrame: the previous frame never ended");
        m_Native->FramePool = objc_autoreleasePoolPush();
        // A minimized window parks here, pumping events, before anything waits on the GPU.
        if (!IsHeadless())
        {
            m_Window->WaitUntilPresentable();
        }

        const auto& frame = AcquireNextFrame();
        ++m_FrameSerial;
        m_Native->AdoptPendingRetires();

        // The slot's fence has been waited, so what its last frame wrote is complete. Announced
        // over a copy, so a callback may register or release one.
        if (!m_Native->FrameRetiredCallbacks.empty())
        {
            const auto callbacks = m_Native->FrameRetiredCallbacks;
            for (const auto& [handle, callback] : callbacks)
            {
                callback(m_Native->CurrentFrameInFlight);
            }
        }

        if (!IsHeadless())
        {
            // The drawable size follows the framebuffer before the next drawable is taken; nothing
            // recorded since the last present holds a drawable, so no device wait is needed.
            if (m_Window->ConsumeFramebufferResized())
            {
                m_RenderExtentChanged = true;
            }
            if (m_RenderExtentChanged)
            {
                m_Window->WaitUntilPresentable();
                UpdateRenderExtent();
            }
            VE_PROFILE_SCOPE("Render/Acquire");
            m_Native->SwapChain->Acquire(*m_Window);
        }

        frame.GetInFlightFence().Reset();
        const auto commandBuffer = frame.GetCommandBuffer();
        commandBuffer->Reset();
        commandBuffer->Begin();

        if (m_GpuTimingSupported)
        {
            VE_PROFILE_SCOPE("Render/TimestampReadback");
            Native& native = *m_Native;
            const u32 slot = native.CurrentFrameInFlight;
            const u32 frameBase = slot * (2 + (2 * Native::MaxGpuScopes));
            // The fence just waited covers every timestamp the slot's last frame wrote.
            if (native.TimestampWritten[slot])
            {
                const vector<string>& names = native.ScopeNames[slot];
                const vector<u64> ticks = ResolveTimestamps(native.TimestampHeap, frameBase,
                                                            2 + static_cast<u32>(names.size() * 2));
                if (ticks[1] > ticks[0])
                {
                    m_GpuFrameTimeMs = static_cast<f32>(static_cast<f64>(ticks[1] - ticks[0]) *
                                                        native.TimestampPeriodNs * 1e-6);
                }
                m_GpuPassTimings = TimingsOf(names, native.ScopeDepths[slot],
                                             vector<u64>(ticks.begin() + 2, ticks.end()), ticks[0],
                                             native.TimestampPeriodNs);
            }
            WriteTimestamp(*commandBuffer, native.TimestampHeap, frameBase);
            native.CurrentScopeNames.clear();
            native.CurrentScopeDepths.clear();
            native.OpenScopeStack.clear();
            m_GpuScopeRecording = true;
        }

        m_Native->FrameCommandsOpen = true;
        VE_PROFILE_SCOPE("Render/FrameSetup");
        DrainSetupCommands(*commandBuffer);
        if (!m_PendingBindlessAcquires.empty())
        {
            BeginGpuScope(*commandBuffer, "Frame Acquires");
            DrainBindlessAcquires(*commandBuffer);
            EndGpuScope(*commandBuffer);
        }
        m_GeneratedTextures->Pump(*commandBuffer);
        m_AsyncReadback->Pump(*commandBuffer);
        return *commandBuffer;
    }

    void Context::EndFrame()
    {
        const auto& frame = GetCurrentFrame();
        const auto commandBuffer = frame.GetCommandBuffer();
        if (!IsHeadless())
        {
            Backend::TransitionImage(*commandBuffer, *GetCurrentSwapChainImage(),
                                     ImageLayout::PresentSrc);
        }
        if (m_GpuTimingSupported)
        {
            Native& native = *m_Native;
            const u32 slot = native.CurrentFrameInFlight;
            WriteTimestamp(*commandBuffer, native.TimestampHeap,
                           (slot * (2 + (2 * Native::MaxGpuScopes))) + 1);
            native.ScopeNames[slot] = std::move(native.CurrentScopeNames);
            native.ScopeDepths[slot] = std::move(native.CurrentScopeDepths);
            native.TimestampWritten[slot] = true;
            m_GpuScopeRecording = false;
        }
        m_Native->FrameCommandsOpen = false;
        commandBuffer->End();
        {
            VE_PROFILE_SCOPE("Render/Submit");
            SubmitFrame(frame);
        }
        {
            VE_PROFILE_SCOPE("Render/Present");
            PresentFrame();
        }
        // Retires from here until the next BeginFrame park in the pending bin.
        m_Native->FrameRecording = false;
        if (m_Native->GpuCaptureFramesLeft > 0 && --m_Native->GpuCaptureFramesLeft == 0)
        {
            [[MTLCaptureManager sharedCaptureManager] stopCapture];
            Log::Info("GPU capture finished");
        }
        objc_autoreleasePoolPop(m_Native->FramePool);
        m_Native->FramePool = nullptr;
    }

    bool Context::IsGpuCaptureAvailable() const
    {
        return [[MTLCaptureManager sharedCaptureManager]
                   supportsDestination:MTLCaptureDestinationGPUTraceDocument] == YES;
    }

    bool Context::IsGpuCaptureActive() const
    {
        return m_Native->GpuCaptureFramesLeft > 0;
    }

    VoidResult Context::CaptureGpuFrames(const path& output, const u32 frameCount)
    {
        if (frameCount == 0)
        {
            return std::unexpected(string("a GPU capture covers at least one frame"));
        }
        if (!IsGpuCaptureAvailable())
        {
            return std::unexpected(string(
                "GPU capture is unavailable: launch the process with MTL_CAPTURE_ENABLED=1"));
        }
        MTLCaptureManager* const manager = [MTLCaptureManager sharedCaptureManager];
        if (m_Native->GpuCaptureFramesLeft > 0 || manager.isCapturing)
        {
            return std::unexpected(string("a GPU capture is already recording"));
        }
        std::error_code exists;
        if (std::filesystem::exists(output, exists))
        {
            return std::unexpected(fmt::format("'{}' already exists", output.string()));
        }
        std::error_code created;
        std::filesystem::create_directories(output.parent_path(), created);

        MTLCaptureDescriptor* const descriptor = [[MTLCaptureDescriptor alloc] init];
        descriptor.captureObject = m_Native->Device;
        descriptor.destination = MTLCaptureDestinationGPUTraceDocument;
        descriptor.outputURL = [NSURL fileURLWithPath:Metal::ToNSString(output.string())];
        NSError* error = nil;
        if (![manager startCaptureWithDescriptor:descriptor error:&error])
        {
            return std::unexpected(
                fmt::format("Metal refused the GPU capture: {}", Metal::Describe(error)));
        }
        m_Native->GpuCaptureFramesLeft = frameCount;
        Log::Info("GPU capture of {} frame(s) to '{}'", frameCount, output.string());
        return {};
    }

    void Context::SubmitFrame(const SynchronizationFrame& frame) const
    {
        const Fence::Native& fence = frame.GetInFlightFence().GetNative();
        const std::scoped_lock lock(m_Native->SubmitMutex);
        // The drawable is handed to the queue before the frame that renders into it commits, and
        // signalled once that frame's work is done.
        if (m_Native->SwapChain)
        {
            m_Native->SwapChain->WaitForDrawable(m_Native->Queue);
        }
        m_Native->Commit(frame.GetCommandBuffer()->GetNative().CommandBuffer, fence.Event,
                         fence.Target, true);
        if (m_Native->SwapChain)
        {
            m_Native->SwapChain->Present(m_Native->Queue);
        }
    }

    void Context::PresentFrame()
    {
        // The drawable was presented with the frame's commit; only the slot advances here.
        m_Native->CurrentFrameInFlight =
            (m_Native->CurrentFrameInFlight + 1) % m_Native->MaxFramesInFlight;
    }

    void Context::AddFrameTransferWait(const TimelineSemaphore& timeline, const u64 value)
    {
        const std::scoped_lock lock(m_Native->SubmitMutex);
        m_Native->PendingFrameTransferWaits.emplace_back(timeline.GetNative().Event, value);
    }

    void Context::SubmitImmediateCommands(CommandBuffer& commandBuffer) const
    {
        // Waits on this submission's own fence, not the whole queue.
        const Unique<Fence> fence = Fence::Create(const_cast<Context&>(*this), "Immediate Submit");
        {
            const std::scoped_lock lock(m_Native->SubmitMutex);
            m_Native->Commit(commandBuffer.GetNative().CommandBuffer, fence->GetNative().Event,
                             fence->GetNative().Target, true);
        }
        fence->Wait();
    }

    void Context::ImmediateCommands(const std::function<void(CommandBuffer&)>& function) const
    {
        // Drains what Metal autoreleases while the recording is made (see Native::FramePool).
        @autoreleasepool
        {
            auto& self = const_cast<Context&>(*this);
            Native& native = *m_Native;
            const auto commandBuffer = CommandBuffer::Create(self);
            commandBuffer->Begin(CommandBufferUsage::OneTimeSubmit);

            // A one-shot recording times its scopes into a heap of its own; a frame left recording is
            // paused so none of this work lands in the frame's stamps. A nested call records untimed.
            const bool wasRecording = self.m_GpuScopeRecording;
            const bool wasImmediate = native.ImmediateScopeRecording;
            const bool timed = m_GpuTimingSupported && !wasImmediate;
            self.m_GpuScopeRecording = false;
            native.ImmediateScopeRecording = timed;
            if (timed)
            {
                native.ImmediateScopeNames.clear();
                native.ImmediateScopeDepths.clear();
                native.ImmediateOpenScopeStack.clear();
            }

            // Outside a frame the current slot's last frame may still read its per-view and material
            // regions, so its fence is waited before the recording takes them over.
            CommandBuffer* const outerImmediate = native.ActiveImmediateCommands;
            const bool outsideFrame = !native.FrameRecording && outerImmediate == nullptr;
            if (outsideFrame && native.Bindless)
            {
                native.SynchronizationFrames[native.CurrentFrameInFlight].GetInFlightFence().Wait();
                native.Bindless->OnImmediateRecordingBegun(native.CurrentFrameInFlight);
            }
            native.ActiveImmediateCommands = commandBuffer.get();
            self.DrainSetupCommands(*commandBuffer);
            // Inside a frame what went resident waits for the next frame: an acquire recorded here
            // would run ahead of commands the frame recorded before it.
            if (outsideFrame)
            {
                self.DrainBindlessAcquires(*commandBuffer);
            }
            function(*commandBuffer);
            native.ActiveImmediateCommands = outerImmediate;

            VE_ASSERT(!timed || native.ImmediateOpenScopeStack.empty(),
                      "ImmediateCommands: {} GPU scope(s) left open by the recording",
                      native.ImmediateOpenScopeStack.size());
            native.ImmediateScopeRecording = wasImmediate;
            self.m_GpuScopeRecording = wasRecording;

            commandBuffer->End();
            SubmitImmediateCommands(*commandBuffer);

            if (timed)
            {
                // The submit has completed, so every written stamp is resolvable.
                self.m_ImmediateGpuPassTimings.clear();
                const vector<string>& names = native.ImmediateScopeNames;
                if (!names.empty())
                {
                    const vector<u64> ticks = ResolveTimestamps(native.ImmediateTimestampHeap, 0,
                                                                static_cast<u32>(names.size() * 2));
                    self.m_ImmediateGpuPassTimings =
                        TimingsOf(names, native.ImmediateScopeDepths, ticks, ticks[0],
                                  native.TimestampPeriodNs);
                }
            }

            // A process submitting only one-off work never reaches a BeginFrame, so what it dropped
            // and released is reclaimed here once nothing else is in flight.
            if (outsideFrame)
            {
                native.DrainPendingRetiresIfIdle();
            }
        }
    }

    u32 Context::GetMaxFramesInFlight() const
    {
        return m_Native->MaxFramesInFlight;
    }

    u32 Context::GetCurrentFrameInFlight() const
    {
        return m_Native->CurrentFrameInFlight;
    }

    // Apple silicon's limits, the same on every Metal 4 device.
    u32 Context::GetMaxImageDimension2D() const
    {
        return 16384;
    }

    u32 Context::GetMaxImageArrayLayers() const
    {
        return 2048;
    }

    u32 Context::GetMaxAttachmentDimension2D() const
    {
        return 16384;
    }

    f32 Context::GetMaxSamplerAnisotropy() const
    {
        return 16.0f;
    }

    uvec2 Context::GetSwapChainExtent() const
    {
        return RequireSwapChain(*m_Native).GetExtent();
    }

    Format Context::GetSwapChainFormat() const
    {
        return RequireSwapChain(*m_Native).GetSurface().EngineFormat;
    }

    DisplayMode Context::GetActiveDisplayMode() const
    {
        return RequireSwapChain(*m_Native).GetSurface().Mode;
    }

    DisplayColorSpace Context::GetActiveDisplayColorSpace() const
    {
        return RequireSwapChain(*m_Native).GetSurface().ColorSpace;
    }

    vector<PresentMode> Context::GetSupportedPresentModes() const
    {
        // A layer waits for the display's refresh or does not; Mailbox has no Metal form.
        if (!m_Native->SwapChain)
        {
            return {PresentMode::Vsync};
        }
        return {PresentMode::Vsync, PresentMode::Immediate};
    }

    void Context::SetRequestedPresentMode(const PresentMode mode)
    {
        if (m_Native->SwapChain)
        {
            m_Native->SwapChain->SetPresentMode(mode);
        }
    }

    Ref<Image> Context::GetCurrentSwapChainImage() const
    {
        return RequireSwapChain(*m_Native).GetCurrentImage();
    }

    Ref<ImageView> Context::GetCurrentSwapChainImageView() const
    {
        return RequireSwapChain(*m_Native).GetCurrentImageView();
    }

    u32 Context::GetSwapChainImageCount() const
    {
        return RequireSwapChain(*m_Native).GetImageCount();
    }

    u32 Context::GetCurrentSwapChainImageIndex() const
    {
        return RequireSwapChain(*m_Native).GetCurrentImageIndex();
    }

    void Context::AddSwapChainInvalidationCallback(std::function<void()> callback)
    {
        // Headless has no swap chain, so nothing can invalidate one.
        if (m_Native->SwapChain)
        {
            m_Native->SwapChain->AddInvalidationCallback(std::move(callback));
        }
    }

    Context::FrameRetiredHandle
    Context::AddFrameRetiredCallback(std::function<void(u32 slot)> callback)
    {
        const u64 handle = ++m_Native->NextFrameRetiredHandle;
        m_Native->FrameRetiredCallbacks.emplace_back(handle, std::move(callback));
        return handle;
    }

    void Context::RemoveFrameRetiredCallback(const FrameRetiredHandle handle)
    {
        std::erase_if(m_Native->FrameRetiredCallbacks,
                      [handle](const auto& entry) { return entry.first == handle; });
    }

    bool Context::IsExternalTextureImportSupported() const
    {
        return true;
    }

    void* Context::GetExternalDevice() const
    {
        return (__bridge void*)m_Native->Device;
    }

    bool Context::IsFrameSplitSupported() const
    {
        return false;
    }

    void Context::SplitFrame(const function<void(const FrameSplitPoint&)>& /*encode*/)
    {
        VE_ASSERT(false, "SplitFrame: the Metal backend does not split a frame");
    }

    void Context::RecordSetupCommands(std::function<void(CommandBuffer&)> commands)
    {
        if (m_Native->ActiveImmediateCommands != nullptr)
        {
            commands(*m_Native->ActiveImmediateCommands);
            return;
        }
        if (m_Native->FrameCommandsOpen)
        {
            commands(GetCurrentCommandBuffer());
            return;
        }
        m_PendingSetupCommands.push_back(std::move(commands));
    }

    void Context::AcquireNextImage(Semaphore& /*semaphore*/)
    {
        // A drawable is waited by the queue, not through a semaphore.
        RequireSwapChain(*m_Native).Acquire(*m_Window);
    }

    void Context::WaitIdle() const
    {
        // Metal has no device-wide wait: the queue signals an event after everything committed
        // so far, and the wait covers the completion feedback of those submissions too.
        u64 value = 0;
        const bool transferQueue = m_Native->TransferQueue != m_Native->Queue;
        {
            const std::scoped_lock lock(m_Native->SubmitMutex);
            value = ++m_Native->IdleValue;
            [m_Native->Queue signalEvent:m_Native->IdleEvent value:value];
            if (transferQueue)
            {
                [m_Native->TransferQueue signalEvent:m_Native->TransferIdleEvent value:value];
            }
        }
        [m_Native->IdleEvent waitUntilSignaledValue:value timeoutMS:UINT64_MAX];
        if (transferQueue)
        {
            [m_Native->TransferIdleEvent waitUntilSignaledValue:value timeoutMS:UINT64_MAX];
        }
        m_Native->WaitFeedback();
    }

    BindlessRegistry& Context::GetBindlessRegistry() const
    {
        return *m_Native->Bindless;
    }

    void Context::UpdateRenderExtent()
    {
        m_RenderExtentChanged = false;
        // Headless: the render extent is the configured one and never changes.
        if (!m_Native->SwapChain)
        {
            return;
        }
        const uvec2 extent{m_Window->GetWidth(), m_Window->GetHeight()};
        Log::Info("Updating extent {}x{}", extent.x, extent.y);
        m_Native->SwapChain->Resize(extent);
        m_RenderExtent = m_Native->SwapChain->GetExtent();
        m_Native->SwapChain->Invalidated();
    }
}
