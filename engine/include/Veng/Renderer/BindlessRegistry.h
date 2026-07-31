#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

/// @brief The global bindless descriptor subsystem.
namespace Veng::Renderer
{
    class Buffer;
    class Context;
    class CommandBuffer;
    class DescriptorSet;
    class DescriptorSetLayout;
    class ImageView;

    /// @brief Slot index into the per-material block buffer (set 0, binding MaterialParamBinding).
    ///
    /// A draw pushes GetCurrentFrameBase() + Index so the shader's
    /// index * MaterialParamStride load reads the current frame-in-flight's region.
    struct MaterialHandle
    {
        /// @brief Sentinel for an unregistered material.
        static constexpr u32 Invalid = ~0u;
        /// @brief Slot in the material block buffer.
        u32 Index = Invalid;
        /// @brief Returns true if the handle names a registered material slot.
        [[nodiscard]] bool IsValid() const { return Index != Invalid; }
    };

    /// @brief Slot index into the sampled-image array (set 0, binding TextureBinding).
    ///
    /// Indexes `texture2D u_Textures[]` in the shader.
    struct TextureHandle
    {
        /// @brief Sentinel for an unregistered texture.
        static constexpr u32 Invalid = ~0u;
        /// @brief Slot in the sampled-image array.
        u32 Index = Invalid;
        /// @brief Returns true if the handle names a registered texture slot.
        [[nodiscard]] bool IsValid() const { return Index != Invalid; }
    };

    /// @brief Slot index into the sampler array (set 0, binding SamplerBinding).
    ///
    /// Indexes `sampler u_Samplers[]` in the shader.
    struct SamplerHandle
    {
        /// @brief Sentinel for an unregistered sampler.
        static constexpr u32 Invalid = ~0u;
        /// @brief Slot in the sampler array.
        u32 Index = Invalid;
        /// @brief Returns true if the handle names a registered sampler slot.
        [[nodiscard]] bool IsValid() const { return Index != Invalid; }
    };

    /// @brief Slot index into the storage-image array (set 0, binding StorageImageBinding).
    ///
    /// Indexes `image2D u_StorageImages[]` in the shader.
    struct StorageImageHandle
    {
        /// @brief Sentinel for an unregistered storage image.
        static constexpr u32 Invalid = ~0u;
        /// @brief Slot in the storage-image array.
        u32 Index = Invalid;
        /// @brief Returns true if the handle names a registered storage image slot.
        [[nodiscard]] bool IsValid() const { return Index != Invalid; }
    };

    /// @brief Slot index into the byte-address storage-buffer array (set 0, binding StorageBufferBinding).
    ///
    /// Indexes `ByteAddressBuffer g_Buffers[]` in the shader: a material loads typed at
    /// a byte offset (`g_Buffers[handle].Load<T>(off)`), the engine staying layout-agnostic.
    /// The bound buffer is a plain (non-dynamic) storage buffer read at full range and
    /// selected by this index — never a dynamic descriptor offset (which mistranslates
    /// inside set 0's Metal argument buffer on MoltenVK), the same discipline the material
    /// param block follows.
    struct StorageBufferHandle
    {
        /// @brief Sentinel for an unregistered storage buffer.
        static constexpr u32 Invalid = ~0u;
        /// @brief Slot in the storage-buffer array.
        u32 Index = Invalid;
        /// @brief Returns true if the handle names a registered storage buffer slot.
        [[nodiscard]] bool IsValid() const { return Index != Invalid; }
    };

    /// @brief A sampler shared by every caller asking for the same sampling description,
    /// paired with the registry slot it occupies.
    struct SharedSampler
    {
        /// @brief The shared sampler object.
        ///
        /// The type is named qualified because the member takes the same name: an unqualified
        /// `Sampler` here would mean the class before this declaration and the member after it,
        /// which is ill-formed however willingly a compiler takes it.
        Ref<Veng::Renderer::Sampler> Sampler;
        /// @brief The slot the sampler occupies in the registry's sampler array.
        SamplerHandle Handle;
    };

    /// @brief The slots still allocatable in each of the registry's arrayed bindings.
    ///
    /// Each field counts the free slots of its array's fixed capacity (the Max* constants on
    /// BindlessRegistry). A slot Release() has queued is not counted until its deferred window
    /// expires, so the figures report what a Register() call can be handed right now.
    struct BindlessCapacity
    {
        /// @brief Free slots in the sampled-image array, of MaxTextures.
        u32 Textures = 0;
        /// @brief Free slots in the sampler array, of MaxSamplers.
        u32 Samplers = 0;
        /// @brief Free slots in the storage-image array, of MaxStorageImages.
        u32 StorageImages = 0;
        /// @brief Free slots in the byte-address storage-buffer array, of MaxStorageBuffers.
        u32 StorageBuffers = 0;
        /// @brief Free slots in the material block table, of MaxMaterials.
        u32 Materials = 0;
    };

    /// @brief The global bindless descriptor set: set 0, reserved in every
    /// PipelineLayout so it can be bound once and never rebound for the rest of a pass.
    ///
    /// Owned by Context — created during Initialize() and destroyed when the context is
    /// torn down — and reachable via Context::GetBindlessRegistry().
    ///
    /// Provides four arrayed, partiallyBound + updateAfterBind bindings (sampled
    /// images, samplers, storage images, byte-address storage buffers). Register() allocates a free-list slot,
    /// writes the resource into that slot, keeps a Ref so the resource cannot dangle
    /// while a live handle still names it, and returns a typed handle. Release()
    /// defers reclaiming the slot until Context::AcquireNextFrame() has cycled past
    /// every frame-in-flight that could still be sampling it (the same window used
    /// for per-frame deferred GPU resource destruction).
    ///
    /// Bind() binds set 0 once per pipeline bind (a pass binds its pipeline, then
    /// the registry's set 0, then issues draws) — not once per draw. Draws select
    /// array elements via push-constant indices carried in the handles above.
    class BindlessRegistry
    {
    public:
        /// @brief Constructs the registry, allocating the descriptor pool, layout, and set.
        /// @param context The owning context.
        explicit BindlessRegistry(Context& context);

        /// @brief Releases the registry's descriptor pool, layout, and set.
        ~BindlessRegistry();

        BindlessRegistry(const BindlessRegistry&) = delete;
        BindlessRegistry& operator=(const BindlessRegistry&) = delete;

        /// @brief Registers a sampled image view and returns its handle.
        [[nodiscard]] TextureHandle Register(const Ref<ImageView>& sampled);

        /// @brief Registers a sampler and returns its handle.
        ///
        /// The slot is the caller's to release. Prefer AcquireSampler, which allocates one slot
        /// per distinct sampling description instead of one per caller.
        [[nodiscard]] SamplerHandle Register(const Ref<Sampler>& sampler);

        /// @brief Returns the shared sampler for a description, creating and registering it on the
        /// first ask and handing the same one back for every description matching it after.
        ///
        /// A sampler is a pure state object — nothing about it varies with the image it reads — so
        /// the distinct descriptions a build uses number in the single digits however many
        /// resources sample through them. Registering one per resource instead scales the sampler
        /// array with the texture array, and the sampler array is by far the smaller of the two;
        /// going through here keeps the live count at the number of distinct descriptions.
        ///
        /// This is for a sampler a shader reaches by handle. A sampler written into an
        /// author-declared descriptor set takes no slot in set 0 and wants none, so it stays on
        /// Sampler::Create — asking here would spend an array slot on a sampler nothing indexes.
        ///
        /// SamplerInfo::Name is a debug label and takes no part in the match, so a shared sampler
        /// carries the name the first caller gave it. Every other field is compared exactly, floats
        /// by their bit pattern, since each of them changes what the GPU does.
        ///
        /// **The registry keeps a shared sampler for the rest of its own life**: the slot is never
        /// released, because no caller holding it can tell whether it is the last. That is what
        /// bounds the array by the number of distinct descriptions rather than by what is alive at
        /// once, and it is why releasing a shared handle is a contract violation — see
        /// Release(SamplerHandle).
        ///
        /// Runs on the thread that drives the registry, the same one Register runs on.
        /// @param info The sampling description; its Name is used only if this ask mints the sampler.
        /// @return The shared sampler and the slot it occupies.
        [[nodiscard]] SharedSampler AcquireSampler(const SamplerInfo& info);

        /// @brief Registers a storage image view and returns its handle.
        [[nodiscard]] StorageImageHandle RegisterStorage(const Ref<ImageView>& storage);

        /// @brief Registers a byte-address storage buffer and returns its handle.
        ///
        /// Experimental, opt-in. The buffer joins the set-0 `g_Buffers[]` array at a free slot,
        /// bound at full range and read by handle index — a material reads it typed on the shader
        /// side (`g_Buffers[handle].Load<T>(off)`), the engine holding it as layout-agnostic bytes.
        /// The registry keeps a Ref so the buffer cannot dangle while a live handle names it.
        /// @param buffer The storage buffer to register.
        /// @return A handle naming the allocated storage-buffer slot.
        [[nodiscard]] StorageBufferHandle Register(const Ref<Buffer>& buffer);

        /// @brief Allocates a material slot and stores its parameter block.
        ///
        /// The block holds the material's whole entry — bindless handle slots and
        /// authored params alike, laid out by reflection; `block` must be <=
        /// MaterialParamStride. Both cache the block CPU-side, mark the slot dirty
        /// for framesInFlight frames, and write the current frame's region directly
        /// into the host-mapped, ring-buffered buffer — no staging, no WaitIdle.
        /// A per-frame UpdateMaterial is cheap and frame-safe.
        /// @param block The serialized parameter block; must be <= MaterialParamStride bytes.
        /// @return A handle naming the allocated material slot.
        [[nodiscard]] MaterialHandle RegisterMaterial(std::span<const std::byte> block);

        /// @brief Rewrites a live material slot's parameter block.
        ///
        /// Same write path as RegisterMaterial: caches the block, marks it dirty for
        /// framesInFlight frames, and writes the current frame's region immediately.
        /// @param handle The handle returned by RegisterMaterial.
        /// @param block  The updated parameter block; must be <= MaterialParamStride bytes.
        void UpdateMaterial(MaterialHandle handle, std::span<const std::byte> block);

        /// @brief Deferred release of a texture handle. A default-constructed (invalid) handle is a no-op.
        void Release(TextureHandle handle);

        /// @brief Deferred release of a sampler handle. A default-constructed (invalid) handle is a no-op.
        ///
        /// The handle must be one Register(const Ref<Sampler>&) returned. A slot AcquireSampler
        /// handed out belongs to the registry and is shared by every caller that asked for the same
        /// description, so releasing one asserts rather than freeing a slot others still index.
        void Release(SamplerHandle handle);

        /// @brief Deferred release of a storage image handle. A default-constructed (invalid) handle is a no-op.
        void Release(StorageImageHandle handle);

        /// @brief Deferred release of a storage buffer handle. A default-constructed (invalid) handle is a no-op.
        void Release(StorageBufferHandle handle);

        /// @brief Deferred release of a material handle. A default-constructed (invalid) handle is a no-op.
        void Release(MaterialHandle handle);

        /// @brief Binds the registry's set 0 at the given bind point.
        ///
        /// Call once per pipeline bind, not per draw.
        /// @param cmd       The command buffer to record the bind into.
        /// @param bindPoint The pipeline bind point (default Graphics).
        void Bind(CommandBuffer& cmd,
                  PipelineBindPoint bindPoint = PipelineBindPoint::Graphics) const;

        /// @brief The base material index of the current frame-in-flight's region in the
        /// ring-buffered material buffer: currentFrameInFlight * MaxMaterials.
        ///
        /// A draw pushes this plus the material slot so the shader's
        /// index * MaterialParamStride load lands in the current frame's region.
        ///
        /// MoltenVK realizes set 0 as a Metal argument buffer; a dynamic storage
        /// descriptor inside it mistranslates, so the per-frame region is selected
        /// by folding the frame base into the pushed material index rather than by a
        /// dynamic descriptor offset. The shader's indexing is unchanged.
        /// @return The base material slot index for the current frame-in-flight.
        [[nodiscard]] u32 GetCurrentFrameBase() const;

        /// @brief Claims the next view slot within the current frame-in-flight, or reports the budget spent.
        ///
        /// Called once per Viewport::Render (per SceneRenderer::Execute), before its
        /// WriteViewConstants / WriteLights, so that two viewports rendering in one frame write
        /// distinct regions and each's draws read its own. OnFrameAcquired resets the slot to the
        /// first each frame.
        ///
        /// A frame wanting more than MaxViewsPerFrame views is a resource limit ordinary content can
        /// reach, not a contract violation, so the claim fails rather than aborting: it returns false,
        /// leaves the current slot alone, and warns once per registry. A caller that is refused
        /// records nothing this frame — writing the shared regions without a slot of its own would
        /// clobber the one another view's draws still read at submit.
        /// @pre Called before WriteViewConstants / WriteLights for this Execute.
        /// @return True when a slot was claimed; false when the frame's view budget is spent.
        [[nodiscard]] bool TryBeginView();

        /// @brief The view slots still claimable in the current frame-in-flight.
        ///
        /// MaxViewsPerFrame minus the slots claimed so far. A driver of many views spends a bounded
        /// budget against this — reserving what the presented viewports need before the captures
        /// claim theirs — rather than discovering the ceiling on a refused claim.
        /// @return The number of slots TryBeginView can still grant this frame.
        [[nodiscard]] u32 GetRemainingViews() const;

        /// @brief Writes the per-frame view-constants block into the current view slot's region
        /// of the shared view-constants buffer.
        ///
        /// Writing the current region is always safe — that frame is not yet submitted, and each
        /// view slot is distinct within the frame. A pass selects the region by pushing
        /// GetCurrentViewConstantsIndex(), folded into the shader's index * ViewConstantsStride
        /// load, exactly as the material block avoids a dynamic descriptor offset inside set 0's
        /// Metal argument buffer on MoltenVK.
        /// @param block The view-constants data; must be <= ViewConstantsStride bytes.
        void WriteViewConstants(std::span<const std::byte> block);

        /// @brief The current view slot's index into the shared view-constants buffer
        /// (frameInFlight * MaxViewsPerFrame + the TryBeginView slot).
        ///
        /// A pass pushes it so the shader's index * ViewConstantsStride load reads this view's
        /// region. Distinct per viewport render, so two viewports in one frame do not collide.
        /// @return The current view-constants index.
        [[nodiscard]] u32 GetCurrentViewConstantsIndex() const;

        /// @brief Writes the per-frame light list into the current view slot's region of the
        /// shared light buffer.
        ///
        /// Writing the current region is always safe — that frame is not yet submitted, each view
        /// slot is distinct, and the whole region is rewritten every Execute (light count and
        /// contents are per-view data). A pass selects the region by folding GetCurrentLightBase()
        /// into its per-light index, exactly as the material block avoids a dynamic descriptor
        /// offset inside set 0's Metal argument buffer on MoltenVK.
        /// @param lights Per-frame light entries; must hold at most MaxLights entries,
        ///               each LightStride bytes.
        void WriteLights(std::span<const std::byte> lights);

        /// @brief The base light index of the current view slot's region in the shared light
        /// buffer: GetCurrentViewConstantsIndex() * MaxLights.
        ///
        /// A pass folds this into its per-light index so the shader's index * LightStride load
        /// lands in this view's region.
        /// @return The base light index for the current view slot.
        [[nodiscard]] u32 GetCurrentLightBase() const;

        /// @brief Writes the per-frame area-light polygon vertices into the current view slot's
        /// region of the shared area-vertex buffer.
        ///
        /// Parallel to WriteLights: Rect and Polygon area lights emit their world-space vertices
        /// here, and each light's Area.y/z record the base index and count into this region. A
        /// vertex is one vec4 (xyz world position). Writing the current region is always safe for
        /// the same reasons WriteLights is.
        /// @param vertices Per-frame area vertices; at most MaxAreaVertices, each AreaVertexStride bytes.
        void WriteAreaVertices(std::span<const std::byte> vertices);

        /// @brief The base area-vertex index of the current view slot's region in the shared
        /// area-vertex buffer: GetCurrentViewConstantsIndex() * MaxAreaVertices.
        ///
        /// A light's packed polygon base index is relative to zero; the shader adds this base so
        /// the load lands in this view's region, exactly as GetCurrentLightBase does for lights.
        /// @return The base area-vertex index for the current view slot.
        [[nodiscard]] u32 GetCurrentAreaVertexBase() const;

        /// @brief The free slots left in each arrayed binding (see BindlessCapacity).
        ///
        /// Every array has a fixed capacity whose exhaustion is fatal, so how much of one is left is
        /// worth being able to read: a diagnostic reports how close a build is running to a cap, and
        /// a subsystem's own case asserts the slots it took come back when it is dropped — the count
        /// is an invariant across a build/teardown pair, whatever the caps happen to be. It is the
        /// arrayed-binding counterpart to GetRemainingViews.
        /// @return The free slot counts, sampled at the moment of the call.
        [[nodiscard]] BindlessCapacity GetFreeSlots() const;

        /// @brief Returns the descriptor set layout for set 0.
        [[nodiscard]] const Ref<DescriptorSetLayout>& GetSet0Layout() const { return m_Layout; }

        /// @brief Called by Context::AcquireNextFrame() to reclaim slots released while
        /// frame-in-flight slot `frameInFlight` was last current.
        /// @param frameInFlight The frame-in-flight index now being made current.
        void OnFrameAcquired(u32 frameInFlight);

        /// @brief Binding index for the sampled-image array.
        static constexpr u32 TextureBinding = 0;
        /// @brief Binding index for the sampler array.
        static constexpr u32 SamplerBinding = 1;
        /// @brief Binding index for the storage-image array.
        static constexpr u32 StorageImageBinding = 2;
        /// @brief Binding index for the byte-address storage-buffer array.
        static constexpr u32 StorageBufferBinding = 3;
        /// @brief Binding index for the material parameter buffer.
        static constexpr u32 MaterialParamBinding = 4;
        /// @brief Binding index for the view-constants buffer.
        static constexpr u32 ViewConstantsBinding = 5;
        /// @brief Binding index for the light buffer.
        static constexpr u32 LightBinding = 6;
        /// @brief Binding index for the area-light polygon-vertex buffer.
        static constexpr u32 AreaVertexBinding = 7;

        /// @brief Maximum registered sampled images.
        static constexpr u32 MaxTextures = 1024;
        /// @brief Maximum registered samplers.
        ///
        /// An eighth of MaxTextures, because a sampler is a pure state object and the array counts
        /// distinct *descriptions* rather than resources: AcquireSampler hands one slot to every
        /// caller wanting the same filtering, addressing and LOD settings, so the occupancy of a
        /// build is the handful of combinations it uses and does not grow with how many textures
        /// load. A caller taking a slot of its own through Register(const Ref<Sampler>&) — a
        /// description built per resource, an experiment — spends against this directly.
        static constexpr u32 MaxSamplers = 128;
        /// @brief Maximum registered storage images.
        static constexpr u32 MaxStorageImages = 512;
        /// @brief Maximum registered byte-address storage buffers.
        static constexpr u32 MaxStorageBuffers = 256;
        /// @brief Maximum registered material slots.
        ///
        /// The table is engine-wide rather than per scene: a consumer holding several worlds open
        /// at once — a transition that keeps the departing world resident while the arriving one
        /// builds, a world rendered to a capture beside the presented one — draws every one of them
        /// from this table, so the budget a single scene appears to need is not the figure that has
        /// to fit. Exhausting it is a fatal assert rather than a soft failure, so the cap is set
        /// where a plausible multi-world consumer stays clear of it. It costs only the parameter
        /// buffer it sizes (framesInFlight * MaxMaterials * MaterialParamStride, a few hundred
        /// kilobytes of host-mapped storage); no descriptor array is indexed by it, and no shader
        /// reads it — a draw is handed a slot index with the frame base already folded in.
        static constexpr u32 MaxMaterials = 512;

        /// @brief The fixed cap on lights the deferred lighting pass loops per pixel.
        ///
        /// The light SSBO holds framesInFlight * MaxViewsPerFrame copies of MaxLights
        /// entries; the pass evaluates the full Cook-Torrance BRDF per light up to the live count.
        static constexpr u32 MaxLights = 16;

        /// @brief The fixed cap on distinct viewport renders sharing one frame-in-flight.
        ///
        /// The view-constants and light buffers are shared across every Viewport, but each
        /// Viewport::Render writes its own camera and lights. Ringing only by frame-in-flight
        /// would let a second viewport's Execute clobber the region the first's draws still read
        /// at submit (both record into one frame's command buffer, reading the final host value).
        /// So each buffer holds framesInFlight * MaxViewsPerFrame regions; TryBeginView advances a
        /// per-frame view slot each Execute, and GetCurrentViewConstantsIndex() folds it in.
        /// The budget covers every view-slot consumer sharing one frame: the presented viewports
        /// (the editor renders one per visible panel), one face per registered scene capture whose
        /// world a view presents, and the sky cubemap bake, whose material and atmosphere passes
        /// each claim six face slots in the frame a sky (re)bakes (twice that on the SH tier, whose
        /// readback bake claims its own six). A region costs ~6 KB (view constants + lights + area
        /// vertices), so the budget is memory the whole ring pays: raising it costs
        /// framesInFlight * that per added slot. Content that wants more views than this in one
        /// frame is spent against rather than aborted on — the captures give way first (see
        /// ViewportCompositor::RenderRegistered), and a refused claim records nothing.
        static constexpr u32 MaxViewsPerFrame = 32;

        /// @brief The fixed byte stride of one material's parameter block in the
        /// MaterialParamBinding ByteAddressBuffer.
        ///
        /// 16-byte aligned for vector loads; one stride is shared across every material
        /// so a single ByteAddressBuffer can hold a different per-material block layout
        /// per shader, read at index * MaterialParamStride. A block exceeding this is a
        /// cook-time error, so the figure is what bounds how much a single material may
        /// describe — twenty-four float4s, which leaves an ordinary fourteen-to-twenty-field
        /// block room to grow rather than making every addition a packing exercise. It costs
        /// framesInFlight * MaxMaterials * this, well under a megabyte of host-mapped storage.
        ///
        /// **Mirrored on the shader side and in the cooker, all of which move together**:
        /// `MaterialParamStride` in Veng/surface.slang, Veng/postprocess.slang, Veng/sky.slang
        /// and Veng/guifill.slang (the four domain contract headers a material includes), and
        /// the cooker's own copy in Importers/MaterialImporter.cpp, which restates it so the
        /// cooker gains no renderer-header dependency.
        static constexpr u32 MaterialParamStride = 384;

        /// @brief The fixed byte stride of one frame-in-flight's view-constants region in
        /// the ViewConstantsBinding ByteAddressBuffer.
        ///
        /// One stride per frame-in-flight; a pass reads at index * ViewConstantsStride.
        /// Mirrors the shader-side constant in view_constants.slang; the ViewConstants
        /// block (the camera/view matrices, the sub-rect mapping, the sky SH, the frame
        /// clock, and the scene-color grab handles) is 560 bytes, within the stride.
        static constexpr u32 ViewConstantsStride = 640;

        /// @brief The fixed byte stride of one GpuLight entry in the LightBinding
        /// ByteAddressBuffer.
        ///
        /// The GpuLight struct (six vec4) is 96 bytes; the pass reads the i-th light
        /// at (base + i) * LightStride. The first four vec4 are the punctual light
        /// fields (position/range, direction/type, color/intensity, cone/flags); the
        /// last two carry the area-light shape (sphere radius, polygon vertex
        /// range, area-shadow slot, and the area normal).
        static constexpr u32 LightStride = 96;

        /// @brief The fixed cap on area-light polygon vertices packed per view.
        ///
        /// Rect and Polygon area lights emit their world-space vertices into the
        /// per-view area-vertex buffer (each vertex one vec4); a light records its
        /// base index and count. A Rect contributes four; a Polygon contributes its
        /// PolygonVertices, clamped so the running total stays within this cap.
        static constexpr u32 MaxAreaVertices = 256;

        /// @brief The byte stride of one packed area-light polygon vertex (a vec4).
        static constexpr u32 AreaVertexStride = 16;

    private:
        /// @brief A free-list slot allocator with deferred release, one per arrayed binding.
        ///
        /// A slot freed while frame-in-flight index i is current goes into PendingRelease[i]
        /// and is only returned to the free list the next time AcquireNextFrame makes index i
        /// current (i.e. after its fence has been waited), by which point no in-flight draw
        /// can still be sampling it.
        struct SlotArray
        {
            /// @brief Per-slot resource Ref keeping the resource alive.
            vector<Ref<void>> Slots;
            /// @brief Available slot indices.
            vector<u32> Free;
            /// @brief Slots pending reclaim, one bucket per frame-in-flight.
            vector<vector<u32>> PendingRelease;

            /// @brief Initializes the arrays for the given capacity and frames-in-flight count.
            void Init(u32 capacity, u32 framesInFlight);
            /// @brief Allocates a slot, storing `resource` as its owner. Asserts capacity.
            u32 Allocate(Ref<void> resource, string_view what);
            /// @brief Queues slot `index` for reclaim once frame `currentFrameInFlight` has completed.
            void ReleaseDeferred(u32 index, u32 currentFrameInFlight);
            /// @brief Reclaims all slots whose deferred window has expired for `frameInFlight`.
            void OnFrameAcquired(u32 frameInFlight);
        };

        /// @brief Writes a sampled image view into the descriptor set at the given texture slot.
        void WriteTexture(u32 index, const Ref<ImageView>& view) const;
        /// @brief Writes a sampler into the descriptor set at the given sampler slot.
        void WriteSampler(u32 index, const Ref<Sampler>& sampler) const;
        /// @brief Writes a storage image view into the descriptor set at the given storage slot.
        void WriteStorageImage(u32 index, const Ref<ImageView>& view) const;
        /// @brief Writes a byte-address storage buffer into the descriptor set at the given buffer slot.
        void WriteStorageBuffer(u32 index, const Ref<Buffer>& buffer) const;

        /// @brief The owning context.
        Context& m_Context;
        /// @brief The descriptor set layout for set 0.
        Ref<DescriptorSetLayout> m_Layout;
        /// @brief The descriptor set for set 0.
        Ref<DescriptorSet> m_Set;

        /// @brief Slot allocator for the sampled-image array.
        SlotArray m_Textures;
        /// @brief Slot allocator for the sampler array.
        SlotArray m_Samplers;

        /// @brief One shared sampler and the description it answers for.
        struct SamplerCacheEntry
        {
            /// @brief The description the sampler was created from.
            SamplerInfo Info;
            /// @brief The shared sampler and its slot.
            SharedSampler Shared;
        };

        /// @brief The shared samplers, one per distinct description AcquireSampler has been asked for.
        ///
        /// Searched linearly. The entry count is the number of distinct descriptions the build uses
        /// and can never exceed MaxSamplers, so a scan is cheaper than hashing a struct of floats
        /// and enumerators — and it needs no hash to agree with the field-by-field equality the
        /// search already applies, which is where a memcmp or a whole-struct hash would go wrong on
        /// the padding between those fields.
        vector<SamplerCacheEntry> m_SharedSamplers;
        /// @brief Slot allocator for the storage-image array.
        SlotArray m_StorageImages;
        /// @brief Slot allocator for the byte-address storage-buffer array.
        SlotArray m_StorageBuffers;

        /// @brief The per-material block buffer (binding MaterialParamBinding).
        ///
        /// A host-visible, persistently-mapped storage buffer holding framesInFlight
        /// copies of the MaxMaterials * MaterialParamStride material table, bound at
        /// its full range. Each frame-in-flight f owns the region
        /// [f * MaxMaterials * MaterialParamStride, ...); a draw folds f's base
        /// (GetCurrentFrameBase()) into the pushed material index so the shader's
        /// index * MaterialParamStride load reads the current frame's copy.
        ///
        /// The binding is a plain (non-dynamic) storage buffer: MoltenVK realizes set 0
        /// as a Metal argument buffer, where a dynamic storage descriptor mistranslates,
        /// so the frame region is selected by the folded index, not a dynamic offset.
        ///
        /// A write only ever touches the current frame's region (that frame is not yet
        /// submitted). To propagate a value to every region, Register/UpdateMaterial cache
        /// the block CPU-side, set the material's dirty counter to framesInFlight, and
        /// write the current region; OnFrameAcquired memcpys each still-dirty material's
        /// block into the region it just made current and decrements. No staging, no
        /// WaitIdle, no frames-in-flight hazard.
        Ref<Buffer> m_MaterialParamBuffer;
        /// @brief Slot allocator for the material parameter block buffer.
        SlotArray m_Materials;
        /// @brief Number of frames-in-flight; determines ring-buffer region count.
        u32 m_FramesInFlight = 0;

        /// @brief CPU-side cache of each material slot's block and its remaining flush count.
        ///
        /// Indexed by material slot. A zero DirtyFrames means the slot is clean across
        /// every region.
        struct MaterialEntry
        {
            /// @brief Cached parameter block bytes.
            vector<u8> Block;
            /// @brief Writes still owed to in-flight regions.
            u32 DirtyFrames = 0;
        };
        vector<MaterialEntry> m_MaterialEntries;

        /// @brief Memcpys a material slot's cached block into the given frame-in-flight's
        /// region of the mapped buffer.
        void WriteMaterialRegion(u32 materialIndex, u32 frameInFlight) const;

        /// @brief The shared view-constants buffer (binding ViewConstantsBinding).
        ///
        /// A host-visible, persistently-mapped storage buffer holding
        /// framesInFlight * MaxViewsPerFrame copies of one ViewConstantsStride region, bound at
        /// its full range. Each (frame f, view slot v) owns the region at index
        /// (f * MaxViewsPerFrame + v) * ViewConstantsStride; a pass pushes that index
        /// (GetCurrentViewConstantsIndex()) so the shader's index * ViewConstantsStride load reads
        /// its own view's region. Like the material block, it is a plain (non-dynamic) storage
        /// buffer selected by a folded index, not a dynamic offset (which mistranslates inside set
        /// 0's Metal argument buffer on MoltenVK). Each view's constants are rewritten every
        /// Execute, and per-view slots keep two viewports in one frame from colliding, so only the
        /// current (not-yet-submitted) region is touched — no fence, no staging.
        Ref<Buffer> m_ViewConstantsBuffer;

        /// @brief The shared light buffer (binding LightBinding).
        ///
        /// A host-visible, persistently-mapped storage buffer holding
        /// framesInFlight * MaxViewsPerFrame copies of the MaxLights * LightStride light table,
        /// bound at its full range. Each (frame f, view slot v) owns the region based at
        /// (f * MaxViewsPerFrame + v) * MaxLights * LightStride; a pass folds that base
        /// (GetCurrentLightBase()) into its per-light index so the shader's index * LightStride
        /// load reads its own view's region. Selected by the folded index, not a dynamic offset
        /// (which mistranslates inside set 0's Metal argument buffer on MoltenVK). The whole
        /// region is rewritten every Execute, per view slot, so only the current
        /// (not-yet-submitted) region is touched.
        Ref<Buffer> m_LightBuffer;

        /// @brief The shared area-light polygon-vertex buffer, ringed like m_LightBuffer.
        ///
        /// Holds framesInFlight * MaxViewsPerFrame * MaxAreaVertices vec4 vertices; a light's
        /// packed polygon base is folded through GetCurrentAreaVertexBase() into its own view's
        /// region, so Rect and Polygon lights read their world-space vertices from here.
        Ref<Buffer> m_AreaVertexBuffer;

        /// @brief Distinct viewport renders seen so far in the current frame-in-flight.
        ///
        /// Reset to 0 by OnFrameAcquired; TryBeginView takes the current slot from it and increments.
        u32 m_ViewsThisFrame = 0;

        /// @brief The view slot the current Execute writes and reads (set by TryBeginView).
        u32 m_ViewSlot = 0;

        /// @brief Latch for the spent-view-budget warning, so it is logged once per registry.
        bool m_ViewBudgetWarned = false;
    };
}
