#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;
    class Sampler;

    struct DescriptorBinding
    {
        u32 Binding = 0;
        DescriptorType Type{};
        u32 Count = 1;
        ShaderStage Stages{};

        // Immutable samplers baked into the layout for this binding (one per
        // descriptor in Count). Set for a SampledImage binding that is sampled
        // through a fixed sampler the layout owns — e.g. a comparison sampler for
        // hardware SampleCmp — so a write supplies only the image, never a sampler.
        // Empty for the common case.
        vector<Ref<Sampler>> ImmutableSamplers{};
        // Static (default, false): written at setup/between frames, then
        // bound — no descriptor-indexing flags. Bindless (true): may be
        // updated while a set is bound (e.g. WriteArray into a streaming
        // table) — opts into UpdateAfterBind/partiallyBound. The engine
        // derives the Vulkan flags from this intent; see
        // DescriptorSetLayout.cpp.
        bool Bindless = false;
    };

    struct DescriptorSetLayoutInfo
    {
        string Name;
        vector<DescriptorBinding> Bindings;
    };

    class DescriptorSetLayout
    {
    public:
        static Ref<DescriptorSetLayout> Create(Context& context, const DescriptorSetLayoutInfo& info)
        {
            return Ref<DescriptorSetLayout>(new DescriptorSetLayout(context, info));
        }

        ~DescriptorSetLayout();

        DescriptorSetLayout(const DescriptorSetLayout&) = delete;
        DescriptorSetLayout& operator=(const DescriptorSetLayout&) = delete;

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] const vector<DescriptorBinding>& GetBindings() const { return m_Bindings; }

        // Look up a binding by its binding *number* (sparse-safe — works for
        // layouts like 0, 2, 5). Both are fatal if the binding does not exist.
        [[nodiscard]] DescriptorType GetBindingType(u32 binding) const;
        [[nodiscard]] u32 GetBindingCount(u32 binding) const; // descriptor count (array size)

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        DescriptorSetLayout(Context& context, const DescriptorSetLayoutInfo& info);

        // The context this resource was created with (deferred-destruction
        // back-ref; a resource must not outlive its context).
        Context& m_Context;
        string m_Name;
        vector<DescriptorBinding> m_Bindings;
        map<u32, DescriptorBinding> m_BindingsByNumber;
        Unique<Native> m_Native;
    };
}
