#include <Veng/Renderer/Context.h>

#include <algorithm>
#include <utility>

#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Log.h>
#include <Veng/Renderer/AsyncReadback.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GeneratedTextureService.h>
#include <Veng/Task/TaskSystem.h>

#include "../../../WindowNative.h"
#include "../../MetalFx.h"
#include "Natives.h"
#include "TypeMapping.h"

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

        [[noreturn]] void NoSwapChain()
        {
            VE_ASSERT(false, "no swapchain (headless)");
            std::abort();
        }
    }

    u64 CreatePresentationSurface(const Context& /*context*/, GLFWwindow* /*window*/)
    {
        VE_ASSERT(false, "the Metal backend runs headless work only; it presents to no window");
        return 0;
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
                    heaps->erase(it);
                }
                return;
            }
        }
        VE_ASSERT(false, "Context: freeing a placement in a heap the context does not hold");
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
                                 const bool waitTransfers)
    {
        // A queue wait holds back every later commit, so a transfer value waited once is settled.
        if (waitTransfers)
        {
            for (const auto& [transferEvent, transferValue] : PendingFrameTransferWaits)
            {
                [Queue waitForEvent:transferEvent value:transferValue];
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
        [Queue commit:buffers count:1 options:options];
        if (event != nil)
        {
            [Queue signalEvent:event value:value];
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
        VE_ASSERT(window == nullptr,
                  "Context: the Metal backend runs headless work only; it presents to no window");
        Log::Info("Initializing Metal context (headless)");
        m_Window = nullptr;
        Native& native = *m_Native;
        native.Headless = true;

        native.Device = MTLCreateSystemDefaultDevice();
        VE_ASSERT(native.Device != nil, "Context: this system has no Metal device");
        VE_ASSERT([native.Device supportsFamily:MTLGPUFamilyMetal4],
                  "Context: '{}' does not support the Metal 4 GPU family the Metal backend needs "
                  "(Apple silicon on macOS 26 or later)",
                  Metal::ToString([native.Device name]));

        native.Queue = [native.Device newMTL4CommandQueue];
        VE_ASSERT(native.Queue != nil, "Context: failed to create the Metal 4 command queue");

        NSError* error = nil;
        MTLResidencySetDescriptor* const residency = [[MTLResidencySetDescriptor alloc] init];
        residency.label = @"Veng Residency";
        residency.initialCapacity = 64;
        native.Residency = [native.Device newResidencySetWithDescriptor:residency error:&error];
        VE_ASSERT(native.Residency != nil, "Context: failed to create the residency set: {}",
                  Metal::Describe(error));
        [native.Queue addResidencySet:native.Residency];

        MTL4CompilerDescriptor* const compiler = [[MTL4CompilerDescriptor alloc] init];
        compiler.label = @"Veng Compiler";
        native.Compiler = [native.Device newCompilerWithDescriptor:compiler error:&error];
        VE_ASSERT(native.Compiler != nil, "Context: failed to create the Metal compiler: {}",
                  Metal::Describe(error));

        native.IdleEvent = [native.Device newSharedEvent];

        // One queue carries everything, so every family is the same one.
        native.QueueFamilies = {.GraphicsFamily = 0u, .PresentFamily = 0u, .TransferFamily = 0u};
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
        native.BlockCompressionSupported = [native.Device supportsBCTextureCompression];
        native.AstcSupported = [native.Device supportsFamily:MTLGPUFamilyApple2];

        const MetalFxSupport metalFx = QueryMetalFxSupport(*this);
        capabilities.MetalFxTemporal = metalFx.Temporal;
        capabilities.MetalFxSpatial = metalFx.Spatial;
        Log::Info("{}", DescribeDeviceCapabilities(capabilities));

        m_RenderExtent = info.HeadlessExtent;
        m_OutputFormat = info.OutputFormat;
        m_DepthFormat = info.DepthFormat;

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
        native.Disposed = true;

        native.TransferTimeline.reset();
        native.SharedHeaps.clear();
        native.PrivateHeaps.clear();
        native.Residency = nil;
        native.Compiler = nil;
        native.Queue = nil;
        native.IdleEvent = nil;
        native.Device = nil;
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
        const auto& pool = m_Native->TransferPools[workerIndex];
        // The worker's one command buffer is reused once its last upload has completed.
        if (pool.LastSubmittedValue != 0)
        {
            m_Native->TransferTimeline->Wait(pool.LastSubmittedValue);
        }
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
                         value, false);
        pool.LastSubmittedValue = value;
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
        return false;
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
#if defined(VE_DEBUG) && VE_DEBUG
        // A named region in a capture; a debug group is pushed between encoders.
        CommandBuffer::Native& native = cmd.GetNative();
        if (native.Recording)
        {
            if (native.Compute != nil)
            {
                [native.Compute endEncoding];
                native.Compute = nil;
            }
            [native.CommandBuffer pushDebugGroup:Metal::ToNSString(name)];
        }
#else
        (void)cmd;
        (void)name;
#endif
    }

    void Context::EndGpuScope(CommandBuffer& cmd)
    {
#if defined(VE_DEBUG) && VE_DEBUG
        CommandBuffer::Native& native = cmd.GetNative();
        if (native.Recording)
        {
            if (native.Compute != nil)
            {
                [native.Compute endEncoding];
                native.Compute = nil;
            }
            [native.CommandBuffer popDebugGroup];
        }
#else
        (void)cmd;
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

        frame.GetInFlightFence().Reset();
        const auto commandBuffer = frame.GetCommandBuffer();
        commandBuffer->Reset();
        commandBuffer->Begin();

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
        m_Native->FrameCommandsOpen = false;
        commandBuffer->End();
        {
            VE_PROFILE_SCOPE("Render/Submit");
            SubmitFrame(frame);
        }
        PresentFrame();
        // Retires from here until the next BeginFrame park in the pending bin.
        m_Native->FrameRecording = false;
    }

    void Context::SubmitFrame(const SynchronizationFrame& frame) const
    {
        const Fence::Native& fence = frame.GetInFlightFence().GetNative();
        const std::scoped_lock lock(m_Native->SubmitMutex);
        m_Native->Commit(frame.GetCommandBuffer()->GetNative().CommandBuffer, fence.Event,
                         fence.Target, true);
    }

    void Context::PresentFrame()
    {
        // Headless: nothing to present, only the slot advances.
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
        auto& self = const_cast<Context&>(*this);
        Native& native = *m_Native;
        const auto commandBuffer = CommandBuffer::Create(self);
        commandBuffer->Begin(CommandBufferUsage::OneTimeSubmit);

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

        commandBuffer->End();
        SubmitImmediateCommands(*commandBuffer);

        // A process submitting only one-off work never reaches a BeginFrame, so what it dropped
        // and released is reclaimed here once nothing else is in flight.
        if (outsideFrame)
        {
            native.DrainPendingRetiresIfIdle();
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
        NoSwapChain();
    }

    Format Context::GetSwapChainFormat() const
    {
        NoSwapChain();
    }

    DisplayMode Context::GetActiveDisplayMode() const
    {
        NoSwapChain();
    }

    DisplayColorSpace Context::GetActiveDisplayColorSpace() const
    {
        NoSwapChain();
    }

    vector<PresentMode> Context::GetSupportedPresentModes() const
    {
        // No surface headless: the always-available mode, so callers get a well-formed list.
        return {PresentMode::Vsync};
    }

    void Context::SetRequestedPresentMode(PresentMode /*mode*/) {}

    Ref<Image> Context::GetCurrentSwapChainImage() const
    {
        NoSwapChain();
    }

    Ref<ImageView> Context::GetCurrentSwapChainImageView() const
    {
        NoSwapChain();
    }

    u32 Context::GetSwapChainImageCount() const
    {
        NoSwapChain();
    }

    u32 Context::GetCurrentSwapChainImageIndex() const
    {
        NoSwapChain();
    }

    void Context::AddSwapChainInvalidationCallback(std::function<void()> /*callback*/)
    {
        // Headless has no swap chain, so nothing can invalidate one.
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
        NoSwapChain();
    }

    void Context::WaitIdle() const
    {
        // Metal has no device-wide wait: the queue signals an event after everything committed
        // so far, and the wait covers the completion feedback of those submissions too.
        u64 value = 0;
        {
            const std::scoped_lock lock(m_Native->SubmitMutex);
            value = ++m_Native->IdleValue;
            [m_Native->Queue signalEvent:m_Native->IdleEvent value:value];
        }
        [m_Native->IdleEvent waitUntilSignaledValue:value timeoutMS:UINT64_MAX];
        m_Native->WaitFeedback();
    }

    BindlessRegistry& Context::GetBindlessRegistry() const
    {
        return *m_Native->Bindless;
    }

    void Context::UpdateRenderExtent()
    {
        // Headless: the render extent is the configured one and never changes.
        m_RenderExtentChanged = false;
    }
}
