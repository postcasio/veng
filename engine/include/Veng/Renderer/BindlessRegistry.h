#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

// The global bindless descriptor subsystem.
namespace Veng::Renderer
{
    class Buffer;
    class Context;
    class CommandBuffer;
    class DescriptorSet;
    class DescriptorSetLayout;
    class ImageView;
    class Sampler;

    // Index into the registry's per-material block buffer (set 0, binding
    // BindlessRegistry::MaterialParamBinding), byte-addressed at index *
    // MaterialParamStride. A draw pushes GetCurrentFrameBase() + Index as the
    // selector so the load lands in the current frame-in-flight's region of the
    // ring-buffered buffer.
    struct MaterialHandle
    {
        static constexpr u32 Invalid = ~0u;
        u32 Index = Invalid;
        [[nodiscard]] bool IsValid() const { return Index != Invalid; }
    };

    // Index into the registry's sampled-image array (set 0, binding
    // BindlessRegistry::TextureBinding). Indexes `texture2D u_Textures[]`.
    struct TextureHandle
    {
        static constexpr u32 Invalid = ~0u;
        u32 Index = Invalid;
        [[nodiscard]] bool IsValid() const { return Index != Invalid; }
    };

    // Index into the registry's sampler array (set 0, binding
    // BindlessRegistry::SamplerBinding). Indexes `sampler u_Samplers[]`.
    struct SamplerHandle
    {
        static constexpr u32 Invalid = ~0u;
        u32 Index = Invalid;
        [[nodiscard]] bool IsValid() const { return Index != Invalid; }
    };

    // Index into the registry's storage-image array (set 0, binding
    // BindlessRegistry::StorageImageBinding). Indexes `image2D u_StorageImages[]`.
    struct StorageImageHandle
    {
        static constexpr u32 Invalid = ~0u;
        u32 Index = Invalid;
        [[nodiscard]] bool IsValid() const { return Index != Invalid; }
    };

    // The global bindless descriptor set: set 0, reserved in every
    // PipelineLayout (see PipelineLayout.cpp) so it can be bound once and
    // never rebound for the rest of a pass. Owned by Context — created during
    // Initialize() and destroyed in Dispose() — and reachable via
    // Context::GetBindlessRegistry().
    //
    // Three arrayed, partiallyBound + updateAfterBind bindings (sampled
    // images, samplers, storage images). Register() allocates a free-list
    // slot, writes the resource into that slot, keeps a Ref so the resource
    // can't dangle while a live handle still names it, and returns a typed
    // handle. Release() defers reclaiming the slot until
    // Context::AcquireNextFrame() has cycled past every frame-in-flight that
    // could still be sampling it (the same window the per-frame retire bins
    // use).
    //
    // Bind() binds set 0 once per pipeline bind (a pass binds its pipeline,
    // then the registry's set 0, then issues draws) — not once per draw.
    // Draws select array elements via push-constant indices carried in the
    // handles above.
    class BindlessRegistry
    {
    public:
        explicit BindlessRegistry(Context& context);
        ~BindlessRegistry();

        BindlessRegistry(const BindlessRegistry&) = delete;
        BindlessRegistry& operator=(const BindlessRegistry&) = delete;

        [[nodiscard]] TextureHandle Register(const Ref<ImageView>& sampled);
        [[nodiscard]] SamplerHandle Register(const Ref<Sampler>& sampler);
        [[nodiscard]] StorageImageHandle RegisterStorage(const Ref<ImageView>& storage);

        // Allocates a material slot and stores its single parameter block. The
        // block holds the material's whole entry — bindless handle slots and
        // authored params alike, laid out by reflection; `block` must be <=
        // MaterialParamStride. UpdateMaterial rewrites a live slot (e.g. after
        // Material::SetTexture/SetParam). Both cache the block CPU-side, mark the
        // slot dirty for framesInFlight frames, and write the current frame's
        // region directly into the host-mapped, ring-buffered buffer — no staging,
        // no WaitIdle. A per-frame UpdateMaterial is cheap and frame-safe. The byte
        // form keeps the registry agnostic of how the material packed its entry.
        [[nodiscard]] MaterialHandle RegisterMaterial(std::span<const std::byte> block);
        void UpdateMaterial(MaterialHandle handle, std::span<const std::byte> block);

        // Deferred release: a default-constructed (invalid) handle is a no-op.
        void Release(TextureHandle handle);
        void Release(SamplerHandle handle);
        void Release(StorageImageHandle handle);
        void Release(MaterialHandle handle);

        // Binds the registry's set 0 at the given bind point. Call once per
        // pipeline bind, not per draw.
        void Bind(CommandBuffer& cmd, PipelineBindPoint bindPoint = PipelineBindPoint::Graphics) const;

        // The base material index of the current frame-in-flight's region in the
        // ring-buffered material buffer: currentFrameInFlight * MaxMaterials. A
        // draw pushes this plus the material slot so the shader's
        // index * MaterialParamStride load lands in the current frame's region.
        //
        // MoltenVK realizes set 0 as a Metal argument buffer; a dynamic storage
        // descriptor inside it mistranslates, so the per-frame region is selected
        // by folding the frame base into the pushed material index rather than by a
        // dynamic descriptor offset. The shader's indexing is unchanged.
        [[nodiscard]] u32 GetCurrentFrameBase() const;

        // Write the per-frame view-constants block into the current frame-in-flight's
        // region of the ring-buffered view-constants buffer. `block` must be <=
        // ViewConstantsStride. Writing the current region is always safe — that
        // frame is not yet submitted. A pass selects this frame's region by pushing
        // GetCurrentViewConstantsIndex(), folded into the shader's
        // index * ViewConstantsStride load, exactly as the material block avoids a
        // dynamic descriptor offset inside set 0's Metal argument buffer.
        void WriteViewConstants(std::span<const std::byte> block);

        // The current frame-in-flight's index into the ring-buffered view-constants
        // buffer (== the frame-in-flight slot). A pass pushes it so the shader's
        // index * ViewConstantsStride load reads this frame's region.
        [[nodiscard]] u32 GetCurrentViewConstantsIndex() const;

        // Write the per-frame light list into the current frame-in-flight's region
        // of the ring-buffered light buffer. `lights` must hold at most MaxLights
        // entries, each LightStride bytes. Writing the current region is always
        // safe — that frame is not yet submitted — and the whole region is rewritten
        // every frame (light count and contents are per-frame data, not topology).
        // A pass selects this frame's region by folding GetCurrentLightBase() into
        // its per-light index, exactly as the material block avoids a dynamic
        // descriptor offset inside set 0's Metal argument buffer.
        void WriteLights(std::span<const std::byte> lights);

        // The base light index of the current frame-in-flight's region in the
        // ring-buffered light buffer: currentFrameInFlight * MaxLights. A pass folds
        // this into its per-light index so the shader's index * LightStride load
        // lands in this frame's region.
        [[nodiscard]] u32 GetCurrentLightBase() const;

        [[nodiscard]] const Ref<DescriptorSetLayout>& GetSet0Layout() const { return m_Layout; }

        // Called by Context::AcquireNextFrame() — reclaims slots released
        // while frame-in-flight slot `frameInFlight` was last current.
        void OnFrameAcquired(u32 frameInFlight);

        static constexpr u32 TextureBinding = 0;
        static constexpr u32 SamplerBinding = 1;
        static constexpr u32 StorageImageBinding = 2;
        static constexpr u32 MaterialParamBinding = 4;
        static constexpr u32 ViewConstantsBinding = 5;
        static constexpr u32 LightBinding = 6;

        static constexpr u32 MaxTextures = 1024;
        static constexpr u32 MaxSamplers = 128;
        static constexpr u32 MaxStorageImages = 512;
        static constexpr u32 MaxMaterials = 256;

        // The fixed cap on lights the deferred lighting pass loops per pixel. The
        // light SSBO holds framesInFlight copies of MaxLights entries; the pass
        // evaluates the full Cook-Torrance BRDF per light up to the live count.
        static constexpr u32 MaxLights = 16;

        // The fixed byte stride of one material's parameter block in the
        // binding-MaterialParamBinding ByteAddressBuffer. 16-byte aligned for
        // vector loads; one stride is shared across every material so a single
        // ByteAddressBuffer can hold a different per-material block layout per
        // shader, read at index * MaterialParamStride. A block exceeding this is a
        // cook-time error.
        static constexpr u32 MaterialParamStride = 256;

        // The fixed byte stride of one frame-in-flight's view-constants region in
        // the binding-ViewConstantsBinding ByteAddressBuffer. One stride per
        // frame-in-flight; a pass reads at index * ViewConstantsStride. The
        // ViewConstants block (InvViewProj + CameraPosition + the directional
        // light-space matrix + shadow params + the SSAO view/projection matrices)
        // is 288 bytes, within the stride.
        static constexpr u32 ViewConstantsStride = 512;

        // The fixed byte stride of one GpuLight entry in the binding-LightBinding
        // ByteAddressBuffer. The GpuLight struct (four vec4) is 64 bytes; the pass
        // reads the i-th light at (base + i) * LightStride.
        static constexpr u32 LightStride = 64;

    private:
        // A free-list slot allocator with deferred release, one per arrayed
        // binding. Mirrors Context::Native::RetireBin: a slot freed while
        // frame-in-flight index i is current goes into PendingRelease[i] and
        // is only returned to the free list the next time AcquireNextFrame
        // makes index i current again (i.e. after its fence has been waited),
        // by which point no in-flight draw can still be sampling it.
        struct SlotArray
        {
            vector<Ref<void>> Slots;
            vector<u32> Free;
            vector<vector<u32>> PendingRelease;

            void Init(u32 capacity, u32 framesInFlight);
            u32 Allocate(Ref<void> resource, string_view what);
            void ReleaseDeferred(u32 index, u32 currentFrameInFlight);
            void OnFrameAcquired(u32 frameInFlight);
        };

        void WriteTexture(u32 index, const Ref<ImageView>& view) const;
        void WriteSampler(u32 index, const Ref<Sampler>& sampler) const;
        void WriteStorageImage(u32 index, const Ref<ImageView>& view) const;

        Context& m_Context;
        Ref<DescriptorSetLayout> m_Layout;
        Ref<DescriptorSet> m_Set;

        SlotArray m_Textures;
        SlotArray m_Samplers;
        SlotArray m_StorageImages;

        // The per-material block buffer (binding MaterialParamBinding): a
        // host-visible, persistently-mapped storage buffer holding framesInFlight
        // copies of the MaxMaterials * MaterialParamStride material table, bound at
        // its full range. Each frame-in-flight f owns the region
        // [f * MaxMaterials * MaterialParamStride, ...); a draw folds f's base
        // (GetCurrentFrameBase()) into the pushed material index so the shader's
        // index * MaterialParamStride load reads the current frame's copy of the
        // block. The binding is a plain (non-dynamic) storage buffer: MoltenVK
        // realizes set 0 as a Metal argument buffer, where a dynamic storage
        // descriptor mistranslates, so the frame region is selected by the folded
        // index, not a dynamic offset.
        //
        // A write only ever touches the *current* frame's region (that frame is
        // not yet submitted, so writing it is safe with no fence). To propagate a
        // value to every region, Register/UpdateMaterial cache the block CPU-side,
        // set the material's dirty counter to framesInFlight, and write the current
        // region; OnFrameAcquired memcpys each still-dirty material's block into the
        // region it just made current and decrements. A cooked (write-once) material
        // reaches all regions over framesInFlight frames; a per-frame-rewritten
        // material stays dirty and lands in each region as it comes current. No
        // staging, no WaitIdle, no frames-in-flight hazard.
        Ref<Buffer> m_MaterialParamBuffer;
        SlotArray m_Materials;
        u32 m_FramesInFlight = 0;

        // CPU-side cache of each material slot's block and its remaining flush
        // count (writes still owed to the in-flight regions). Indexed by material
        // slot. A zero DirtyFrames means the slot is clean across every region.
        struct MaterialEntry
        {
            vector<u8> Block;
            u32 DirtyFrames = 0;
        };
        vector<MaterialEntry> m_MaterialEntries;

        // memcpy a material slot's cached block into the given frame-in-flight's
        // region of the mapped buffer.
        void WriteMaterialRegion(u32 materialIndex, u32 frameInFlight) const;

        // The per-frame view-constants buffer (binding ViewConstantsBinding): a
        // host-visible, persistently-mapped storage buffer holding framesInFlight
        // copies of one ViewConstantsStride region, bound at its full range. Each
        // frame-in-flight f owns the region [f * ViewConstantsStride, ...); a pass
        // pushes f (GetCurrentViewConstantsIndex()) so the shader's
        // index * ViewConstantsStride load reads the current frame's region. Like
        // the material block, it is a plain (non-dynamic) storage buffer selected by
        // a folded index, not a dynamic offset (which mistranslates inside set 0's
        // Metal argument buffer on MoltenVK). A frame's view constants are rewritten
        // every Execute, so only the current (not-yet-submitted) region is touched —
        // no fence, no staging.
        Ref<Buffer> m_ViewConstantsBuffer;

        // The per-frame light buffer (binding LightBinding): a host-visible,
        // persistently-mapped storage buffer holding framesInFlight copies of the
        // MaxLights * LightStride light table, bound at its full range. Each
        // frame-in-flight f owns the region [f * MaxLights * LightStride, ...); a
        // pass folds f's base (GetCurrentLightBase()) into its per-light index so
        // the shader's index * LightStride load reads the current frame's region.
        // Selected by the folded index, not a dynamic offset (which mistranslates
        // inside set 0's Metal argument buffer on MoltenVK). The whole region is
        // rewritten every Execute, so only the current (not-yet-submitted) region is
        // touched.
        Ref<Buffer> m_LightBuffer;
    };
}
