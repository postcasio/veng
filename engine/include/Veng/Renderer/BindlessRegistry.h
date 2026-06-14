#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

// The global bindless descriptor subsystem (planset-5/05). See
// plans/future/bindless-descriptors.md for the design and
// plans/planset-5/05-bindless.md for the plan that landed it.
namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class DescriptorSet;
    class DescriptorSetLayout;
    class ImageView;
    class Sampler;

    // Index into the registry's sampled-image array (set 0, binding
    // BindlessRegistry::k_TextureBinding). Indexes `texture2D u_Textures[]`.
    struct TextureHandle
    {
        static constexpr u32 k_Invalid = ~0u;
        u32 Index = k_Invalid;
        [[nodiscard]] bool IsValid() const { return Index != k_Invalid; }
    };

    // Index into the registry's sampler array (set 0, binding
    // BindlessRegistry::k_SamplerBinding). Indexes `sampler u_Samplers[]`.
    struct SamplerHandle
    {
        static constexpr u32 k_Invalid = ~0u;
        u32 Index = k_Invalid;
        [[nodiscard]] bool IsValid() const { return Index != k_Invalid; }
    };

    // Index into the registry's storage-image array (set 0, binding
    // BindlessRegistry::k_StorageImageBinding). Indexes `image2D u_StorageImages[]`.
    struct StorageImageHandle
    {
        static constexpr u32 k_Invalid = ~0u;
        u32 Index = k_Invalid;
        [[nodiscard]] bool IsValid() const { return Index != k_Invalid; }
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

        // Deferred release: a default-constructed (invalid) handle is a no-op.
        void Release(TextureHandle handle);
        void Release(SamplerHandle handle);
        void Release(StorageImageHandle handle);

        // Binds the registry's set 0 at the given bind point. Call once per
        // pipeline bind, not per draw.
        void Bind(CommandBuffer& cmd, PipelineBindPoint bindPoint = PipelineBindPoint::Graphics) const;

        [[nodiscard]] const Ref<DescriptorSetLayout>& GetSet0Layout() const { return m_Layout; }

        // Called by Context::AcquireNextFrame() — reclaims slots released
        // while frame-in-flight slot `frameInFlight` was last current.
        void OnFrameAcquired(u32 frameInFlight);

        static constexpr u32 k_TextureBinding = 0;
        static constexpr u32 k_SamplerBinding = 1;
        static constexpr u32 k_StorageImageBinding = 2;

        static constexpr u32 k_MaxTextures = 1024;
        static constexpr u32 k_MaxSamplers = 128;
        static constexpr u32 k_MaxStorageImages = 512;

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
    };
}
