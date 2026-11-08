#pragma once

// The Metal binding model, device-free: where each descriptor binding of a set lands in its
// argument buffer, and which buffer slot each dynamic uniform buffer of a pipeline layout binds at.
// The cook assigns the same placement to a shader's reflected bindings (Veng/binding.slang), so a
// layout built from a binding list agrees with every shader declaring that set.

#include <algorithm>
#include <span>

#include <Veng/Veng.h>

#include <Veng/Assert.h>
#include <Veng/Cook/MetalBindingSlots.h>
#include <Veng/Renderer/DescriptorSetLayout.h>

namespace Veng::Renderer::Metal
{
    /// @brief The argument-buffer elements one descriptor of @p type takes.
    ///
    /// A combined image sampler is the texture then the sampler; a dynamic uniform buffer is bound
    /// by address outside the argument buffer and takes none.
    /// @param type The descriptor type.
    /// @return The elements per array entry.
    [[nodiscard]] constexpr u32 ElementsPerDescriptor(const DescriptorType type)
    {
        switch (type)
        {
        case DescriptorType::CombinedImageSampler:
            return 2;
        case DescriptorType::UniformBufferDynamic:
            return 0;
        case DescriptorType::SampledImage:
        case DescriptorType::StorageImage:
        case DescriptorType::UniformBuffer:
        case DescriptorType::StorageBuffer:
        case DescriptorType::Sampler:
            return 1;
        }
        return 0;
    }

    /// @brief Where one binding of a set lands on Metal.
    struct BindingPlacement
    {
        /// @brief The binding number.
        u32 Binding = 0;
        /// @brief The binding's descriptor type.
        DescriptorType Type{};
        /// @brief The binding's array count.
        u32 Count = 1;
        /// @brief The binding's first argument-buffer element; meaningless for a dynamic buffer.
        u32 FirstElement = 0;
        /// @brief For a dynamic uniform buffer, its position among the set's dynamic buffers.
        u32 DynamicIndex = 0;
    };

    /// @brief One set's placement: every binding, in binding order.
    struct SetPlacement
    {
        /// @brief The bindings, sorted by binding number.
        vector<BindingPlacement> Bindings;
        /// @brief The argument buffer's element count.
        u32 ElementCount = 0;
        /// @brief The set's dynamic uniform buffers.
        u32 DynamicCount = 0;

        /// @brief Finds a binding's placement.
        /// @param binding The binding number.
        /// @return The placement, or null when the set has no such binding.
        [[nodiscard]] const BindingPlacement* Find(const u32 binding) const
        {
            for (const BindingPlacement& placement : Bindings)
            {
                if (placement.Binding == binding)
                {
                    return &placement;
                }
            }
            return nullptr;
        }
    };

    /// @brief Places a set's bindings: consecutive elements in binding order, each binding
    ///        ElementsPerDescriptor × Count of them, dynamic uniform buffers apart.
    ///
    /// A shader declares a set as one struct whose fields are its bindings in order, with every
    /// dynamic uniform buffer after them, so a dynamic buffer before an argument-buffer binding is
    /// fatal: no shader could declare it.
    /// @param name     The layout's name, for the messages.
    /// @param bindings The set's bindings, in any order.
    /// @return The placement.
    [[nodiscard]] inline SetPlacement PlaceSet(const string_view name,
                                               const std::span<const DescriptorBinding> bindings)
    {
        SetPlacement placement;
        placement.Bindings.reserve(bindings.size());
        for (const DescriptorBinding& binding : bindings)
        {
            placement.Bindings.push_back(
                {.Binding = binding.Binding, .Type = binding.Type, .Count = binding.Count});
        }
        std::ranges::sort(placement.Bindings, {}, &BindingPlacement::Binding);

        bool seenDynamic = false;
        for (usize i = 0; i < placement.Bindings.size(); ++i)
        {
            BindingPlacement& entry = placement.Bindings[i];
            VE_ASSERT(i == 0 || placement.Bindings[i - 1].Binding != entry.Binding,
                      "DescriptorSetLayout '{}': binding {} is declared twice", name,
                      entry.Binding);
            if (entry.Type == DescriptorType::UniformBufferDynamic)
            {
                VE_ASSERT(entry.Count == 1,
                          "DescriptorSetLayout '{}': dynamic uniform buffer {} is arrayed; Metal "
                          "binds each one at a buffer slot of its own",
                          name, entry.Binding);
                entry.DynamicIndex = placement.DynamicCount++;
                seenDynamic = true;
                continue;
            }
            VE_ASSERT(!seenDynamic,
                      "DescriptorSetLayout '{}': binding {} follows a dynamic uniform buffer; a "
                      "shader declares every dynamic buffer after its set's other bindings",
                      name, entry.Binding);
            entry.FirstElement = placement.ElementCount;
            placement.ElementCount += ElementsPerDescriptor(entry.Type) * entry.Count;
        }
        return placement;
    }

    /// @brief The buffer slot of each dynamic uniform buffer across a pipeline layout's sets, in
    ///        set then binding order.
    /// @param layoutName The pipeline layout's name, for the message.
    /// @param sets       Each set's dynamic buffer count, in set order.
    /// @return For each set, the slot of its first dynamic buffer.
    [[nodiscard]] inline vector<u32> DynamicBufferSlots(const string_view layoutName,
                                                        const std::span<const u32> sets)
    {
        namespace Slots = Cook::MetalBindingSlots;
        VE_ASSERT(sets.size() <= Slots::MaxDescriptorSets,
                  "PipelineLayout '{}': {} descriptor sets, past the {} Metal binds", layoutName,
                  sets.size(), Slots::MaxDescriptorSets);
        vector<u32> firstSlots;
        firstSlots.reserve(sets.size());
        u32 ordinal = 0;
        for (const u32 count : sets)
        {
            firstSlots.push_back(Slots::DynamicUniformBufferBase + ordinal);
            ordinal += count;
        }
        VE_ASSERT(ordinal <= Slots::MaxDynamicUniformBuffers,
                  "PipelineLayout '{}': {} dynamic uniform buffers, past the {} Metal binds",
                  layoutName, ordinal, Slots::MaxDynamicUniformBuffers);
        return firstSlots;
    }
}
