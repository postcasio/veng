#pragma once
#include <span>

#include <Veng/Renderer/Backend/Buffer.h>
#include <Veng/Renderer/Backend/DescriptorSetLayout.h>
#include <Veng/Renderer/Backend/ImageView.h>
#include <Veng/Renderer/Backend/Sampler.h>

namespace Veng::Renderer
{
    struct DescriptorSetInfo
    {
        string Name;
        Ref<DescriptorSetLayout> Layout;
    };

    class DescriptorSet
    {
    public:
        static Ref<DescriptorSet> Create(const DescriptorSetInfo& info)
        {
            return CreateRef<DescriptorSet>(info);
        }

        explicit DescriptorSet(const DescriptorSetInfo& info);
        ~DescriptorSet();

        // Typed writes. The descriptor type is inferred from the layout's
        // binding — the caller never restates it. A payload kind that doesn't
        // match the binding's type, or a binding the layout doesn't have, is a
        // fatal assert naming the binding and type. Each write updates the set
        // immediately and retains the written resources so a bound view/buffer
        // cannot dangle while the set is still bound in a future frame.

        // Combined image sampler.
        void Write(u32 binding, const Ref<ImageView>& view, const Ref<Sampler>& sampler);
        // Sampled image or storage image (disambiguated by the layout; the
        // image layout follows from the type).
        void Write(u32 binding, const Ref<ImageView>& view);
        // Uniform or storage buffer, whole range.
        void Write(u32 binding, const Ref<Buffer>& buffer);
        // Uniform or storage buffer, explicit range.
        void Write(u32 binding, const Ref<Buffer>& buffer, u64 offset, u64 range);
        // Array of combined image samplers (bindless-style).
        void WriteArray(u32 binding, std::span<const Ref<ImageView>> views,
                        const Ref<Sampler>& sampler, u32 firstElement = 0);

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] vk::DescriptorSet GetVkDescriptorSet() const { return m_DescriptorSet; }

    private:
        string m_Name;
        vk::DescriptorSet m_DescriptorSet;
        // Ownership of the resources currently written into each binding, keyed
        // by binding number. Re-writing a binding releases exactly what it
        // replaced. This is the ownership list (dangling-descriptor
        // prevention), distinct from the per-frame retire queue of plan 04.
        map<u32, vector<Ref<void>>> m_BoundPerBinding;
        Ref<DescriptorSetLayout> m_Layout;
    };
}
