#pragma once

#include <span>

#include <Veng/Veng.h>
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
    class Sampler;

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

    /// @brief The global bindless descriptor set: set 0, reserved in every
    /// PipelineLayout so it can be bound once and never rebound for the rest of a pass.
    ///
    /// Owned by Context — created during Initialize() and destroyed in Dispose() —
    /// and reachable via Context::GetBindlessRegistry().
    ///
    /// Provides three arrayed, partiallyBound + updateAfterBind bindings (sampled
    /// images, samplers, storage images). Register() allocates a free-list slot,
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
        [[nodiscard]] SamplerHandle Register(const Ref<Sampler>& sampler);

        /// @brief Registers a storage image view and returns its handle.
        [[nodiscard]] StorageImageHandle RegisterStorage(const Ref<ImageView>& storage);

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
        void Release(SamplerHandle handle);

        /// @brief Deferred release of a storage image handle. A default-constructed (invalid) handle is a no-op.
        void Release(StorageImageHandle handle);

        /// @brief Deferred release of a material handle. A default-constructed (invalid) handle is a no-op.
        void Release(MaterialHandle handle);

        /// @brief Binds the registry's set 0 at the given bind point.
        ///
        /// Call once per pipeline bind, not per draw.
        /// @param cmd       The command buffer to record the bind into.
        /// @param bindPoint The pipeline bind point (default Graphics).
        void Bind(CommandBuffer& cmd, PipelineBindPoint bindPoint = PipelineBindPoint::Graphics) const;

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

        /// @brief Writes the per-frame view-constants block into the current frame-in-flight's
        /// region of the ring-buffered view-constants buffer.
        ///
        /// Writing the current region is always safe — that frame is not yet submitted.
        /// A pass selects this frame's region by pushing GetCurrentViewConstantsIndex(),
        /// folded into the shader's index * ViewConstantsStride load, exactly as the
        /// material block avoids a dynamic descriptor offset inside set 0's Metal argument
        /// buffer on MoltenVK.
        /// @param block The view-constants data; must be <= ViewConstantsStride bytes.
        void WriteViewConstants(std::span<const std::byte> block);

        /// @brief The current frame-in-flight's index into the ring-buffered view-constants
        /// buffer (== the frame-in-flight slot).
        ///
        /// A pass pushes it so the shader's index * ViewConstantsStride load reads this
        /// frame's region.
        /// @return The current frame-in-flight index.
        [[nodiscard]] u32 GetCurrentViewConstantsIndex() const;

        /// @brief Writes the per-frame light list into the current frame-in-flight's region
        /// of the ring-buffered light buffer.
        ///
        /// Writing the current region is always safe — that frame is not yet submitted —
        /// and the whole region is rewritten every frame (light count and contents are
        /// per-frame data). A pass selects this frame's region by folding
        /// GetCurrentLightBase() into its per-light index, exactly as the material block
        /// avoids a dynamic descriptor offset inside set 0's Metal argument buffer on
        /// MoltenVK.
        /// @param lights Per-frame light entries; must hold at most MaxLights entries,
        ///               each LightStride bytes.
        void WriteLights(std::span<const std::byte> lights);

        /// @brief The base light index of the current frame-in-flight's region in the
        /// ring-buffered light buffer: currentFrameInFlight * MaxLights.
        ///
        /// A pass folds this into its per-light index so the shader's index * LightStride
        /// load lands in this frame's region.
        /// @return The base light index for the current frame-in-flight.
        [[nodiscard]] u32 GetCurrentLightBase() const;

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
        /// @brief Binding index for the material parameter buffer.
        static constexpr u32 MaterialParamBinding = 4;
        /// @brief Binding index for the view-constants buffer.
        static constexpr u32 ViewConstantsBinding = 5;
        /// @brief Binding index for the light buffer.
        static constexpr u32 LightBinding = 6;

        /// @brief Maximum registered sampled images.
        static constexpr u32 MaxTextures = 1024;
        /// @brief Maximum registered samplers.
        static constexpr u32 MaxSamplers = 128;
        /// @brief Maximum registered storage images.
        static constexpr u32 MaxStorageImages = 512;
        /// @brief Maximum registered material slots.
        static constexpr u32 MaxMaterials = 256;

        /// @brief The fixed cap on lights the deferred lighting pass loops per pixel.
        ///
        /// The light SSBO holds framesInFlight copies of MaxLights entries; the pass
        /// evaluates the full Cook-Torrance BRDF per light up to the live count.
        static constexpr u32 MaxLights = 16;

        /// @brief The fixed byte stride of one material's parameter block in the
        /// MaterialParamBinding ByteAddressBuffer.
        ///
        /// 16-byte aligned for vector loads; one stride is shared across every material
        /// so a single ByteAddressBuffer can hold a different per-material block layout
        /// per shader, read at index * MaterialParamStride. A block exceeding this is a
        /// cook-time error.
        static constexpr u32 MaterialParamStride = 256;

        /// @brief The fixed byte stride of one frame-in-flight's view-constants region in
        /// the ViewConstantsBinding ByteAddressBuffer.
        ///
        /// One stride per frame-in-flight; a pass reads at index * ViewConstantsStride.
        /// The ViewConstants block (InvViewProj + CameraPosition + the directional
        /// light-space matrix + shadow params + the SSAO view/projection matrices)
        /// is 288 bytes, within the stride.
        static constexpr u32 ViewConstantsStride = 512;

        /// @brief The fixed byte stride of one GpuLight entry in the LightBinding
        /// ByteAddressBuffer.
        ///
        /// The GpuLight struct (four vec4) is 64 bytes; the pass reads the i-th light
        /// at (base + i) * LightStride.
        static constexpr u32 LightStride = 64;

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
        /// @brief Slot allocator for the storage-image array.
        SlotArray m_StorageImages;

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

        /// @brief The per-frame view-constants buffer (binding ViewConstantsBinding).
        ///
        /// A host-visible, persistently-mapped storage buffer holding framesInFlight
        /// copies of one ViewConstantsStride region, bound at its full range. Each
        /// frame-in-flight f owns the region [f * ViewConstantsStride, ...); a pass
        /// pushes f (GetCurrentViewConstantsIndex()) so the shader's
        /// index * ViewConstantsStride load reads the current frame's region. Like the
        /// material block, it is a plain (non-dynamic) storage buffer selected by a folded
        /// index, not a dynamic offset (which mistranslates inside set 0's Metal argument
        /// buffer on MoltenVK). A frame's view constants are rewritten every Execute, so
        /// only the current (not-yet-submitted) region is touched — no fence, no staging.
        Ref<Buffer> m_ViewConstantsBuffer;

        /// @brief The per-frame light buffer (binding LightBinding).
        ///
        /// A host-visible, persistently-mapped storage buffer holding framesInFlight
        /// copies of the MaxLights * LightStride light table, bound at its full range.
        /// Each frame-in-flight f owns the region [f * MaxLights * LightStride, ...); a
        /// pass folds f's base (GetCurrentLightBase()) into its per-light index so the
        /// shader's index * LightStride load reads the current frame's region. Selected by
        /// the folded index, not a dynamic offset (which mistranslates inside set 0's Metal
        /// argument buffer on MoltenVK). The whole region is rewritten every Execute, so
        /// only the current (not-yet-submitted) region is touched.
        Ref<Buffer> m_LightBuffer;
    };
}
