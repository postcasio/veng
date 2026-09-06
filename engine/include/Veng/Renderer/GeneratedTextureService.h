#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/Types.h>

namespace Veng
{
    class DerivedDataCache;
    class TaskSystem;
}

namespace Veng::Renderer
{
    class Buffer;
    class CommandBuffer;
    class Context;
    class GeneratedTextureQueue;
    class ImageView;

    /// @brief A caller-chosen identity for a generated-texture job and the textures it produced.
    ///
    /// Requests are idempotent on it, so a caller may re-request the same key every frame while it
    /// still wants the result and get exactly one job.
    using GeneratedTextureKey = u64;

    /// @brief One image a job fills, described the way the caller wants it created.
    struct GeneratedTextureTargetInfo
    {
        /// @brief The image to create — any format, extent, layer count, mip count, or type.
        ///
        /// Ignored when Adopt is set. ImageUsage::Sampled and the usage ProducerAccess implies are
        /// folded in, so a caller declares only the extras it needs (TransferSrc for a readback).
        ImageInfo Image;

        /// @brief An already-created image to fill instead of creating one.
        ///
        /// The service takes a reference for the job's lifetime but does not otherwise own it, and
        /// asserts nothing about its usage — an adopted image is the caller's to size correctly.
        Ref<Veng::Renderer::Image> Adopt;

        /// @brief How the job's ticks write this target.
        ///
        /// The service transitions the target into this before the first tick and re-prepares it
        /// between ticks, so tick N+1's writes wait on tick N's; on completion it transitions to
        /// Sample. A compute-filled target is StorageWrite; a face-rendered one ColorAttachment.
        AccessKind ProducerAccess = AccessKind::StorageWrite;

        /// @brief The view type of the sampled view handed back on completion.
        ///
        /// Unset resolves from the image: Type3D for a volume, Array2D for a layered image, Type2D
        /// otherwise. A consumer binding a 6-layer target as a cube through its own descriptor set
        /// names Cube here.
        optional<ImageViewType> SampledViewType;

        /// @brief Whether to register the sampled view in the bindless registry.
        ///
        /// Set 0's sampled-image array is strictly 2D, so this is legal only for a single-layer
        /// Type2D target; the handle is minted on the main thread once the target exists and is
        /// stable for the target's lifetime, but the texels it names are undefined until the job
        /// completes.
        bool Bindless = false;

        /// @brief The mip level of the cached shape this target's own mip 0 restores from.
        ///
        /// Zero — the default — means the target and the stored shape are one image. A nonzero value
        /// declares this target the **coarse tail** of a larger stored chain: the entry matches when
        /// the stored shape reduced by this many mip levels is exactly this target's shape, and the
        /// restore copies the stored levels from this one onward. A consumer that generates a chain
        /// at full resolution but holds only its coarse levels in memory reads its own entry back
        /// this way instead of storing a second entry per resolution it may want to hold.
        ///
        /// Two consequences follow from a tail holding part of a chain rather than a whole one. The
        /// job **restores but never stores** — writing a fragment back would replace the entry with
        /// less than it holds — so a tail target is only ever useful against an entry some other job
        /// wrote at full shape. And its levels are read as a byte range of the payload, so the
        /// entry's digest is not verified (DerivedDataCache::ReadRange says what that costs); a
        /// full-shape restore still verifies it.
        ///
        /// Inert on a job with no cache key, and on a miss: the ticks then run and fill the target at
        /// its own shape, exactly as they would have with no cache attached.
        u32 CacheMipOffset = 0;
    };

    /// @brief One image a job fills, resolved: the image, its views, and its bindless slot.
    ///
    /// Owned by the service for as long as the job is live. A cancelled job's targets are released
    /// with it; a completed job's are held until Release() drops them.
    class GeneratedTexture
    {
    public:
        /// @brief Layer selector meaning "every layer", for GetView.
        static constexpr u32 AllLayers = ~0u;

        /// @brief Releases the image, its views, and its bindless slot through the retire path.
        ~GeneratedTexture();

        GeneratedTexture(const GeneratedTexture&) = delete;
        GeneratedTexture& operator=(const GeneratedTexture&) = delete;

        /// @brief The image the job's ticks fill.
        [[nodiscard]] const Ref<Image>& GetImage() const { return m_Image; }

        /// @brief The view over the whole image — every mip, every layer.
        ///
        /// This is what a completed job hands the caller to sample, and what the service's own
        /// layout transitions are recorded against.
        [[nodiscard]] const Ref<ImageView>& GetSampledView() const { return m_Sampled; }

        /// @brief A view of one mip level, of one layer or of all of them, created on first ask.
        ///
        /// A tick binds these: as a storage image for a compute dispatch, or as a color attachment
        /// for a raster tick — a view is a view, and which it is follows from how the tick uses it.
        /// Views are cached, so asking for the same subresource twice hands back the same view.
        /// @param mipLevel The mip level the view exposes.
        /// @param layer    The array layer, or AllLayers for the whole layer range.
        /// @return The cached view.
        [[nodiscard]] const Ref<ImageView>& GetView(u32 mipLevel, u32 layer = AllLayers) const;

        /// @brief The set-0 slot the sampled view occupies, invalid unless Bindless was asked for.
        [[nodiscard]] TextureHandle GetTextureHandle() const { return m_Handle; }

        /// @brief The access the job's ticks write this target through.
        [[nodiscard]] AccessKind GetProducerAccess() const { return m_ProducerAccess; }

    private:
        friend class GeneratedTextureService;

        GeneratedTexture(Context& context, const GeneratedTextureTargetInfo& info);

        /// @brief Takes the set-0 slot the target asked for, if it asked for one.
        ///
        /// Split out of construction because the image, its view and its sampler are created off
        /// the frame thread while the registry is main-thread-only.
        void RegisterBindless();

        /// @brief The context views are created on and the bindless slot is released to.
        Context& m_Context;
        /// @brief The created or adopted image.
        Ref<Image> m_Image;
        /// @brief The whole-image view.
        Ref<ImageView> m_Sampled;
        /// @brief The access the ticks write through.
        AccessKind m_ProducerAccess;
        /// @brief Whether the target asked for a set-0 slot.
        bool m_WantsBindless = false;
        /// @brief The bindless slot, invalid when the target is not registered.
        TextureHandle m_Handle;

        /// @brief One lazily-created subresource view and the (mip, layer) pair it answers for.
        struct CachedView
        {
            /// @brief The mip level the view exposes.
            u32 MipLevel = 0;
            /// @brief The array layer, or AllLayers.
            u32 Layer = 0;
            /// @brief The view itself.
            Ref<ImageView> View;
        };

        /// @brief The subresource views asked for so far, scanned linearly (a handful at most).
        mutable vector<CachedView> m_Views;
    };

    /// @brief The per-tick state a job's tick callback records against.
    struct GeneratedTextureTickContext
    {
        /// @brief The job's key.
        GeneratedTextureKey Key = 0;
        /// @brief This tick's index within the job, counting from zero.
        u32 TickIndex = 0;
        /// @brief The job's total tick count.
        u32 TickCount = 1;
        /// @brief The job's targets, in the order they were requested.
        std::span<const Unique<GeneratedTexture>> Targets;
    };

    /// @brief A completed job's key and the textures it filled.
    ///
    /// The span is valid until the job is released or the service is destroyed.
    struct GeneratedTextureResult
    {
        /// @brief The job's key.
        GeneratedTextureKey Key = 0;
        /// @brief The filled targets, in the order they were requested.
        std::span<const Unique<GeneratedTexture>> Targets;
    };

    /// @brief A job: the images to fill, how many ticks that takes, and what each tick records.
    struct GeneratedTextureRequest
    {
        /// @brief The caller's key. Requests are idempotent on it.
        GeneratedTextureKey Key = 0;
        /// @brief Debug name. Each created target is named `Name[i]`, and its views after that.
        ///
        /// An adopted target keeps the name it was created with.
        string Name = "GeneratedTexture";
        /// @brief The images the job fills; at least one.
        vector<GeneratedTextureTargetInfo> Targets;
        /// @brief The key this job's result is cached under, or empty for a job that is not cached.
        ///
        /// Set it and the service probes the attached cache before running a single tick: a hit
        /// uploads the stored texels and completes the job with the tick callback never invoked, a
        /// miss runs the job normally and stores its targets once they are filled. The key is the
        /// caller's to compose and must name everything the result depends on that the cache's
        /// generation does not.
        ///
        /// It is inert with no cache attached, so a consumer may set it unconditionally. A target
        /// the service creates has the transfer usages a store and a restore need folded in; an
        /// adopted target carrying neither is simply not cached, and the job logs why.
        /// @see GeneratedTextureService::SetCache
        string CacheKey;
        /// @brief The number of ticks the job takes; the amortization quantum.
        ///
        /// The caller sizes a tick so one is a bounded slice of GPU work — one solver step, one
        /// cube face, one mip of a downsample chain.
        u32 TickCount = 1;
        /// @brief The cost one of this job's ticks charges against the per-frame budget.
        ///
        /// A relative weight the caller declares, not a measured time: the engine cannot know how
        /// dear one tile of a heavy volumetric march is against one star-point splat, so the request
        /// says. The pump spends a total cost budget per frame, so a Cost of K runs a job at roughly
        /// a Kth the ticks-per-frame of a baseline job. The default of one reproduces the pump's
        /// prior tick-count schedule exactly, so a job that declares no cost is unchanged.
        u32 Cost = 1;
        /// @brief Higher runs first; ties break by request order.
        i32 Priority = 0;
        /// @brief Records one tick's GPU work — compute dispatches, a raster pass, or both.
        ///
        /// Called on the render thread with the frame's command buffer, between the service's own
        /// layout transitions. It may not request or cancel a job.
        function<void(CommandBuffer&, const GeneratedTextureTickContext&)> OnTick;
        /// @brief Fired on the main thread once every tick has run and the targets are sampleable.
        ///
        /// Runs after the pump's tick loop, so it may request or cancel further jobs.
        function<void(const GeneratedTextureResult&)> OnComplete;
    };

    /// @brief What the service is holding right now, for a diagnostic panel.
    struct GeneratedTextureStats
    {
        /// @brief Jobs requested whose first tick has not run.
        u32 Queued = 0;
        /// @brief Jobs part-way through their ticks.
        u32 Running = 0;
        /// @brief Jobs whose targets are filled and held.
        u32 Resident = 0;
        /// @brief Ticks run by the most recent pump.
        u32 TicksLastPump = 0;
        /// @brief Jobs waiting on a cache probe, which spend no ticks while they wait.
        u32 Probing = 0;
        /// @brief Jobs waiting on their targets to be created, which spend no ticks while they wait.
        u32 Allocating = 0;
        /// @brief Jobs completed over the service's lifetime.
        u64 CompletedTotal = 0;
        /// @brief Jobs cancelled before completing, over the service's lifetime.
        u64 CancelledTotal = 0;
        /// @brief Jobs completed from the cache without running a tick.
        u64 RestoredTotal = 0;
        /// @brief Job results written to the cache.
        u64 StoredTotal = 0;
    };

    /// @brief Persistent GPU textures filled by caller-recorded jobs the frame amortizes.
    ///
    /// A job is `{target images, tick count, tick cost, tick callback, priority}`. The service
    /// creates (or adopts) the targets, and each frame runs ticks across the pending jobs in
    /// priority-then-FIFO order until their summed cost reaches a per-frame cost budget, recording
    /// them into the frame's command buffer with the layout transitions between a target's producer
    /// ticks and its final sampled state inserted around them. When a job's ticks are exhausted its
    /// targets are transitioned to a sampled layout, its completion fires, and it becomes queryable
    /// as resident.
    ///
    /// **A frame never waits.** There are no immediate submits, no fences on the render thread, and
    /// no job is started outside the pump. An approach faster than a job degrades to "the result
    /// lands a moment later", never to a hitch: the caller keeps drawing whatever it drew before.
    /// That extends to the targets themselves: a target image is created on a task-system worker
    /// and the job is held until it lands, so a request for a large mip-mapped target costs the
    /// frame a submit rather than the allocation. Only the set-0 registration a Bindless target
    /// asks for runs on the main thread, once the image exists.
    ///
    /// **The service makes no policy decisions.** What to generate, when, at what resolution, and
    /// how long to hold it belong to the caller entirely; the service schedules and owns lifetime.
    ///
    /// **A result can outlive the process.** Attach a DerivedDataCache and a job carrying a
    /// CacheKey is answered from disk when the same result was computed on an earlier run: the
    /// probe and the store both run on task-system workers, so neither the render thread nor a
    /// frame ever waits on a file. The cache is transparent — a hit is a texture that arrives
    /// without the job's ticks running, a miss is the job running exactly as if no cache existed,
    /// and a deleted cache directory is a valid state at any moment.
    ///
    /// Owned by Context and pumped once per frame from BeginFrame with the service's own cost
    /// budget, so every consumer of one context shares one budget. It records around the
    /// RenderGraph rather than through it — the graph's transients are 2D single-layer and its
    /// compiled schedule is static between rebuilds, neither of which suits a per-frame-varying set
    /// of layered, mipped targets.
    class GeneratedTextureService
    {
    public:
        /// @brief The default per-frame cost budget.
        ///
        /// With every request at the default Cost of one, this spends this many ticks per frame —
        /// the tick-count budget the cost budget replaced.
        static constexpr u32 DefaultCostBudget = 4;

        /// @brief A budget meaning "run every pending tick each frame" — amortization off.
        static constexpr u32 UnlimitedCostBudget = ~0u;

        /// @brief Constructs the service for a context.
        /// @param context The render context targets are created on.
        explicit GeneratedTextureService(Context& context);

        /// @brief Releases every job's targets through the retire path.
        ~GeneratedTextureService();

        GeneratedTextureService(const GeneratedTextureService&) = delete;
        GeneratedTextureService& operator=(const GeneratedTextureService&) = delete;

        /// @brief Requests a job, idempotently on its key.
        ///
        /// A key already live — queued, running, or resident — is left exactly as it is and the
        /// request is dropped, so re-requesting each frame is the intended usage. A job with a
        /// target to create is held while a worker creates it and runs its first tick at the pump
        /// after that lands; a job whose targets are all adopted has nothing to allocate and runs
        /// at the next pump. Cancel and Release are safe against an allocation still in flight.
        /// @param request The job to run.
        /// @return True when the job was added; false when the key was already live or the request
        ///         named no targets or no tick callback.
        bool Request(GeneratedTextureRequest request);

        /// @brief Tears a job down, whatever its state, releasing its targets. No completion fires.
        /// @param key The job's key.
        /// @return True when a job was cancelled.
        bool Cancel(GeneratedTextureKey key);

        /// @brief Drops a job and its targets without counting it as a cancellation.
        ///
        /// The verb a caller reaches for when it is done with a *completed* result; on an
        /// unfinished job it does exactly what Cancel does.
        /// @param key The job's key.
        /// @return True when a job was released.
        bool Release(GeneratedTextureKey key);

        /// @brief Raises or lowers a live job's priority.
        /// @param key      The job's key.
        /// @param priority The new priority; higher runs first.
        /// @return True when a job was found.
        bool SetPriority(GeneratedTextureKey key, i32 priority);

        /// @brief Whether a key names a job whose targets are filled.
        [[nodiscard]] bool IsResident(GeneratedTextureKey key) const;

        /// @brief Whether a key names a job that is queued or running.
        [[nodiscard]] bool IsPending(GeneratedTextureKey key) const;

        /// @brief The filled targets of a resident job, or nullopt while it is not resident.
        /// @param key The job's key.
        /// @return The result, whose target span is valid until the job is released.
        [[nodiscard]] optional<GeneratedTextureResult> Find(GeneratedTextureKey key) const;

        /// @brief Attaches the cache a keyed job is answered from and stored into, or detaches it.
        ///
        /// Both arguments travel together: the cache says where results live and the task system
        /// says which workers the file I/O runs on, and neither is usable without the other, so
        /// passing null for either detaches. Detaching leaves jobs already in flight alone — a
        /// probe that has been submitted still resolves — and simply stops answering or storing
        /// anything after that.
        ///
        /// The service never opens or owns the cache: what generation it holds and how big it may
        /// grow are the caller's decisions, made where the caller knows what its results depend on.
        /// @param cache  The cache, or null to detach.
        /// @param tasks  The task system probes and stores run on, or null to detach.
        void SetCache(DerivedDataCache* cache, TaskSystem* tasks);

        /// @brief The attached cache, or null when none is.
        [[nodiscard]] DerivedDataCache* GetCache() const { return m_Cache; }

        /// @brief Sets the summed tick cost the pump may spend per frame across every job.
        /// @param costPerFrame The budget, or UnlimitedCostBudget for no amortization.
        void SetCostBudget(u32 costPerFrame) { m_CostBudget = costPerFrame; }

        /// @brief The summed tick cost the pump may spend per frame.
        [[nodiscard]] u32 GetCostBudget() const { return m_CostBudget; }

        /// @brief What the service is holding right now.
        [[nodiscard]] GeneratedTextureStats GetStats() const;

    private:
        friend class Context;

        /// @brief Spends up to @p budget of summed tick cost across the pending jobs, in
        ///        priority-then-FIFO order.
        ///
        /// Called once per frame by Context::BeginFrame with the frame's command buffer, before any
        /// pass records. Completions fire after the tick loop, so a job requested from one is
        /// scheduled from the next pump.
        /// @param cmd    The frame's command buffer the ticks are recorded into.
        /// @param budget Maximum summed tick cost to spend this frame.
        void Pump(CommandBuffer& cmd, u32 budget);

        /// @brief Names the task system the service's own off-thread work runs on.
        ///
        /// Threaded in by Context once the application's task system exists. With none the service
        /// creates targets inline on the calling thread, which is the device-free/test posture.
        /// @param tasks The application's task system, or null.
        void SetTaskSystem(TaskSystem* tasks) { m_ContextTasks = tasks; }

        /// @brief A cache hit's texels, staged and waiting for the pump to copy them into place.
        struct PendingRestore
        {
            /// @brief The host-mapped buffer holding every target's levels.
            Ref<Buffer> Staging;
            /// @brief Byte offset of each target's levels within the staging buffer.
            vector<u64> TargetOffsets;
            /// @brief Whether the worker copying the payload into the buffer has finished.
            bool Staged = false;
        };

        /// @brief A completed job's texels, read back into one buffer and waiting to be written out.
        ///
        /// It holds no reference to the job, so releasing a result while its levels are still in
        /// flight neither strands the store nor dangles: the result was computed, and it is stored.
        struct PendingStore
        {
            /// @brief The key the payload is stored under.
            string CacheKey;
            /// @brief The images read back, in the order the job declared them.
            ///
            /// Held rather than their shapes because a shape is a renderer-internal type, and an
            /// image's shape is immutable, so re-deriving it when the levels land is free.
            vector<Ref<Image>> Images;
            /// @brief The host-mapped buffer every target's levels are copied into.
            Ref<Buffer> Staging;
            /// @brief Texel bytes across every target, which is the buffer's size.
            usize Bytes = 0;
            /// @brief The pump the copies were recorded into.
            u64 StagedPump = 0;
        };

        /// @brief A job's GPU half: the targets it fills and the callbacks that fill and finish it.
        struct Job
        {
            /// @brief The caller's key.
            GeneratedTextureKey Key = 0;
            /// @brief Distinguishes this job from a later one re-using the same key.
            ///
            /// A probe resolving after its job was cancelled and the key re-requested would
            /// otherwise restore one job's texels into another's targets.
            u64 Serial = 0;
            /// @brief The images the job fills.
            vector<Unique<GeneratedTexture>> Targets;
            /// @brief Records one tick's GPU work.
            function<void(CommandBuffer&, const GeneratedTextureTickContext&)> OnTick;
            /// @brief Fired once the job's ticks are exhausted.
            function<void(const GeneratedTextureResult&)> OnComplete;
            /// @brief The cache key the result is stored under; empty when the job is not cached.
            string CacheKey;
            /// @brief Each target's declared mip level within the cached shape, in target order.
            vector<u32> CacheMipOffsets;
            /// @brief Whether any target is a tail of the cached chain rather than the whole of it.
            ///
            /// The job then reads its levels as byte ranges of the entry and never stores its own.
            bool CacheTail = false;
            /// @brief Whether the worker creating the job's targets has yet to report back.
            bool Allocating = false;
            /// @brief Whether a probe of the cache is in flight.
            bool Probing = false;
            /// @brief Whether a hit's texels are still being copied into their staging buffer.
            bool Restoring = false;
            /// @brief A hit's staged texels, applied by the pump once they are staged.
            optional<PendingRestore> Restore;
        };

        /// @brief The task system the service's off-thread work runs on, or null when it has none.
        [[nodiscard]] TaskSystem* GetWorkers() const
        {
            return m_Tasks != nullptr ? m_Tasks : m_ContextTasks;
        }

        /// @brief The job for a key, or null when the key is not live.
        [[nodiscard]] Job* FindJob(GeneratedTextureKey key);
        /// @brief The job for a key, or null when the key is not live.
        [[nodiscard]] const Job* FindJob(GeneratedTextureKey key) const;

        /// @brief Drops a job and its targets, counting it as cancelled only when asked.
        /// @param key             The job's key.
        /// @param countCancelled  Whether to count the drop against CancelledTotal.
        /// @return True when a job was dropped.
        bool Drop(GeneratedTextureKey key, bool countCancelled);

        /// @brief Holds a job for as long as anything it is waiting on is still outstanding.
        ///
        /// One flag on the queue record carries every reason a job is not selectable — its targets
        /// still being created, a cache probe in flight, a hit's texels still being staged — so the
        /// reasons compose and a job is never briefly selectable at the seam between two of them.
        /// @param job The job whose hold to re-derive.
        void UpdateHold(const Job& job);

        /// @brief Submits the worker that creates a job's targets, holding it until they land.
        /// @param job      The job to allocate for; left held and marked allocating.
        /// @param targets  The target descriptions, moved onto the worker.
        void SubmitAllocation(Job& job, vector<GeneratedTextureTargetInfo> targets);

        /// @brief Adopts a resolved allocation's targets, or drops them when the job is gone.
        /// @param key      The allocating job's key.
        /// @param serial   The allocating job's serial; a mismatch drops the targets.
        /// @param targets  The created targets.
        void ResolveAllocation(GeneratedTextureKey key, u64 serial,
                               vector<Unique<GeneratedTexture>> targets);

        /// @brief Starts a job whose targets exist: probes the cache for it, or releases it to run.
        /// @param job The job whose targets are created and registered.
        void BeginJob(Job& job);

        /// @brief Submits the worker probe of the cache for a job, holding it until the answer.
        /// @param job The job to probe for; left held and marked probing.
        void SubmitProbe(Job& job);

        /// @brief Applies a resolved probe: stages a hit's texels, or releases the job to run.
        /// @param key      The probed job's key.
        /// @param serial   The probed job's serial; a mismatch drops the answer.
        /// @param payload  The stored payload, or nullopt on a miss.
        void ResolveProbe(GeneratedTextureKey key, u64 serial, optional<vector<u8>> payload);

        /// @brief Applies a tail job's resolved probe, which read the entry's header alone.
        ///
        /// The tail path's counterpart to ResolveProbe: the shapes are matched against targets that
        /// are mip tails of them, and the levels each target wants are then read as byte ranges of
        /// the entry rather than sliced out of a payload already in memory.
        /// @param key     The probed job's key.
        /// @param serial  The probed job's serial; a mismatch drops the answer.
        /// @param header  The entry's leading bytes, or nullopt on a miss.
        void ResolveTailProbe(GeneratedTextureKey key, u64 serial, optional<vector<u8>> header);

        /// @brief Marks a hit's staging buffer filled, so the next pump copies it into the targets.
        /// @param key     The restoring job's key.
        /// @param serial  The restoring job's serial; a mismatch drops the answer.
        void ResolveRestore(GeneratedTextureKey key, u64 serial);

        /// @brief Records a completed job's targets into one staging buffer for a later store.
        /// @param cmd The frame's command buffer the copies are recorded into.
        /// @param job The job whose targets are filled.
        void SubmitStore(CommandBuffer& cmd, const Job& job);

        /// @brief Hands every readable staged store to a worker that encodes and writes it.
        void FlushStores();

        /// @brief Copies every staged restore into its targets and marks those jobs resident.
        /// @param cmd       The frame's command buffer the copies are recorded into.
        /// @param restored  Receives the keys whose completions the caller must fire.
        void ApplyRestores(CommandBuffer& cmd, vector<GeneratedTextureKey>& restored);

        /// @brief The context targets are created on.
        Context& m_Context;
        /// @brief The device-free scheduling core.
        Unique<GeneratedTextureQueue> m_Queue;
        /// @brief The live jobs' GPU halves, keyed alongside the queue's records.
        ///
        /// Held by pointer so a job's address survives another being added or dropped — a
        /// completion may request the next job, and the tick loop re-finds by key around it.
        vector<Unique<Job>> m_Jobs;
        /// @brief Completed jobs' texels on their way to the cache, in staging order.
        vector<PendingStore> m_Stores;
        /// @brief Pumps run so far; a store's copies are readable framesInFlight pumps after theirs.
        u64 m_PumpCount = 0;
        /// @brief Summed tick cost the pump may spend per frame.
        u32 m_CostBudget = DefaultCostBudget;
        /// @brief Ticks the most recent pump ran.
        u32 m_TicksLastPump = 0;
        /// @brief Jobs completed over the service's lifetime.
        u64 m_CompletedTotal = 0;
        /// @brief Jobs cancelled before completing, over the service's lifetime.
        u64 m_CancelledTotal = 0;
        /// @brief Jobs completed from the cache without running a tick.
        u64 m_RestoredTotal = 0;
        /// @brief Job results written to the cache.
        u64 m_StoredTotal = 0;
        /// @brief True while a caller's tick callback is running; requests and cancels are barred.
        bool m_InTick = false;
        /// @brief The cache keyed jobs are answered from, or null when none is attached.
        DerivedDataCache* m_Cache = nullptr;
        /// @brief The task system the cache's file I/O runs on, or null when none is attached.
        TaskSystem* m_Tasks = nullptr;
        /// @brief The task system Context threaded in, used when no cache supplies one.
        TaskSystem* m_ContextTasks = nullptr;
        /// @brief Monotonic source for Job::Serial.
        u64 m_NextSerial = 1;
    };
}
