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
        /// @brief Ticks spent by the most recent pump.
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
    /// A job is `{target images, tick count, tick callback, priority}`. The service creates (or
    /// adopts) the targets, and each frame runs up to a tick budget of ticks across the pending
    /// jobs in priority-then-FIFO order, recording them into the frame's command buffer with the
    /// layout transitions between a target's producer ticks and its final sampled state inserted
    /// around them. When a job's ticks are exhausted its targets are transitioned to a sampled
    /// layout, its completion fires, and it becomes queryable as resident.
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
    /// Owned by Context and pumped once per frame from BeginFrame with the service's own tick
    /// budget, so every consumer of one context shares one budget. It records around the
    /// RenderGraph rather than through it — the graph's transients are 2D single-layer and its
    /// compiled schedule is static between rebuilds, neither of which suits a per-frame-varying set
    /// of layered, mipped targets.
    class GeneratedTextureService
    {
    public:
        /// @brief The default per-frame tick budget.
        static constexpr u32 DefaultTickBudget = 4;

        /// @brief A budget meaning "run every pending tick each frame" — amortization off.
        static constexpr u32 UnlimitedTickBudget = ~0u;

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

        /// @brief Sets the ticks the pump may spend per frame across every job.
        /// @param ticksPerFrame The budget, or UnlimitedTickBudget for no amortization.
        void SetTickBudget(u32 ticksPerFrame) { m_TickBudget = ticksPerFrame; }

        /// @brief The ticks the pump may spend per frame.
        [[nodiscard]] u32 GetTickBudget() const { return m_TickBudget; }

        /// @brief What the service is holding right now.
        [[nodiscard]] GeneratedTextureStats GetStats() const;

    private:
        friend class Context;

        /// @brief Spends up to @p budget ticks across the pending jobs, in priority-then-FIFO order.
        ///
        /// Called once per frame by Context::BeginFrame with the frame's command buffer, before any
        /// pass records. Completions fire after the tick loop, so a job requested from one is
        /// scheduled from the next pump.
        /// @param cmd    The frame's command buffer the ticks are recorded into.
        /// @param budget Maximum ticks to spend this frame.
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
            /// @brief Whether the worker creating the job's targets has yet to report back.
            bool Allocating = false;
            /// @brief Whether a probe of the cache is in flight.
            bool Probing = false;
            /// @brief A hit's staged texels, applied by the next pump.
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
        /// One flag on the queue record carries every reason a job is not selectable, so the
        /// reasons compose: a job whose targets are still being created and which then probes the
        /// cache is never briefly selectable at the seam between the two.
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

        /// @brief Reads a completed job's targets back and stores them under its cache key.
        /// @param job The job whose targets are filled.
        void SubmitStore(const Job& job);

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
        /// @brief Ticks the pump may spend per frame.
        u32 m_TickBudget = DefaultTickBudget;
        /// @brief Ticks the most recent pump spent.
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
