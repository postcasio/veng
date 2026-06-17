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

    // The engine-supplied per-material block in the registry's MaterialData SSBO
    // array (set 0, binding BindlessRegistry::MaterialBinding) — the bindless
    // texture/sampler handle slots the loader patches. A draw selects its entry
    // with a push-constant materialIndex and the shader reads
    // g_Materials[pc.materialIndex]. This block holds only handle slots; an
    // author's scalar/vector uniforms live in the separate variable-size authored
    // block (set 0, binding MaterialParamBinding). libveng knows this block's
    // layout without reflection — the loader asserts
    // CookedMaterialHeader::EngineBytes == sizeof(MaterialData).
    struct MaterialData
    {
        u32 Albedo = 0;        // bindless sampled-image index
        u32 AlbedoSampler = 0; // bindless sampler index
        u32 Pad0 = 0;
        u32 Pad1 = 0;
    };
    static_assert(sizeof(MaterialData) == 16,
        "MaterialData is the engine-supplied block — handle slots; std430 16-byte stride");

    // Index into the registry's MaterialData SSBO array (set 0, binding
    // BindlessRegistry::MaterialBinding). Pushed per draw as materialIndex.
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

        // Allocates a material slot and uploads its two parameter blocks into it:
        // the engine block (`engine`, == sizeof(MaterialData), into binding
        // MaterialBinding at index * sizeof(MaterialData)) and the authored block
        // (`authored`, <= MaterialParamStride, into binding MaterialParamBinding at
        // index * MaterialParamStride). UpdateMaterial rewrites a live slot in
        // place (e.g. after Material::SetTexture/SetParam). The byte form keeps the
        // registry agnostic of how the material packed its entry.
        [[nodiscard]] MaterialHandle RegisterMaterial(
            std::span<const std::byte> engine, std::span<const std::byte> authored);
        void UpdateMaterial(MaterialHandle handle,
            std::span<const std::byte> engine, std::span<const std::byte> authored) const;

        // Deferred release: a default-constructed (invalid) handle is a no-op.
        void Release(TextureHandle handle);
        void Release(SamplerHandle handle);
        void Release(StorageImageHandle handle);
        void Release(MaterialHandle handle);

        // Binds the registry's set 0 at the given bind point. Call once per
        // pipeline bind, not per draw.
        void Bind(CommandBuffer& cmd, PipelineBindPoint bindPoint = PipelineBindPoint::Graphics) const;

        [[nodiscard]] const Ref<DescriptorSetLayout>& GetSet0Layout() const { return m_Layout; }

        // Called by Context::AcquireNextFrame() — reclaims slots released
        // while frame-in-flight slot `frameInFlight` was last current.
        void OnFrameAcquired(u32 frameInFlight);

        static constexpr u32 TextureBinding = 0;
        static constexpr u32 SamplerBinding = 1;
        static constexpr u32 StorageImageBinding = 2;
        static constexpr u32 MaterialBinding = 3;
        static constexpr u32 MaterialParamBinding = 4;

        static constexpr u32 MaxTextures = 1024;
        static constexpr u32 MaxSamplers = 128;
        static constexpr u32 MaxStorageImages = 512;
        static constexpr u32 MaxMaterials = 256;

        // The fixed byte stride of one material's authored-param block in the
        // binding-MaterialParamBinding ByteAddressBuffer. 16-byte aligned for
        // vector loads; one stride is shared across every material so a single
        // ByteAddressBuffer can hold a different MaterialParams layout per shader,
        // read at index * MaterialParamStride. An authored block exceeding this is
        // a cook-time error.
        static constexpr u32 MaterialParamStride = 256;

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

        // The engine-block SSBO (binding MaterialBinding): a single storage buffer
        // of MaxMaterials * sizeof(MaterialData), written into set 0 once at
        // construction. m_Materials allocates slots into it (deferred release, like
        // the arrays above); per-slot data is uploaded at index *
        // sizeof(MaterialData).
        Ref<Buffer> m_MaterialBuffer;
        SlotArray m_Materials;

        // The authored-block buffer (binding MaterialParamBinding): a single
        // storage buffer of MaxMaterials * MaterialParamStride, written into set 0
        // once at construction, shared with m_Materials' slot allocation (one slot
        // index addresses both buffers). A material's authored params upload at
        // index * MaterialParamStride.
        Ref<Buffer> m_MaterialParamBuffer;
    };
}
