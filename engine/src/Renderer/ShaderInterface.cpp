#include <Veng/Renderer/ShaderInterface.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/DescriptorSetLayout.h>

#include <fmt/format.h>

#include <algorithm>

namespace Veng::Renderer
{
    optional<ShaderBinding> ShaderInterface::FindBinding(std::string_view name) const
    {
        for (const ShaderBinding& binding : Bindings)
        {
            if (binding.Name == name)
            {
                return binding;
            }
        }

        return std::nullopt;
    }

    vector<PushConstantRange> ShaderInterface::BuildPushConstantRanges() const
    {
        vector<PushConstantRange> ranges;
        ranges.reserve(PushConstants.size());

        for (const ShaderPushConstant& pushConstant : PushConstants)
        {
            ranges.push_back(PushConstantRange{
                .Stages = pushConstant.Stages,
                .Offset = pushConstant.Offset,
                .Size = pushConstant.Size,
            });
        }

        return ranges;
    }

    vector<vector<DescriptorBinding>> ShaderInterface::GroupBindingsBySet() const
    {
        if (Bindings.empty())
        {
            return {};
        }

        // Sets [0, FirstUserSet) are the typed bindless registries, prepended to every pipeline
        // layout; author-declared sets begin at FirstUserSet.
        constexpr u32 base = BindlessRegistry::FirstUserSet;

        u32 maxSet = 0;
        for (const ShaderBinding& binding : Bindings)
        {
            VE_ASSERT(binding.Set >= base,
                      "ShaderInterface::GroupBindingsBySet: binding '{}' targets set {} — "
                      "sets 0-{} are reserved for the typed bindless registries",
                      binding.Name, binding.Set, base - 1);
            maxSet = std::max(maxSet, binding.Set);
        }

        // bindingsBySet[i] holds the bindings declared for set (i + base).
        vector<vector<DescriptorBinding>> bindingsBySet(maxSet - base + 1);
        for (const ShaderBinding& binding : Bindings)
        {
            bindingsBySet[binding.Set - base].push_back(DescriptorBinding{
                .Binding = binding.Binding,
                .Type = binding.Type,
                .Count = binding.Count,
                .Stages = binding.Stages,
            });
        }

        for (u32 set = base; set <= maxSet; ++set)
        {
            VE_ASSERT(!bindingsBySet[set - base].empty(),
                      "ShaderInterface::GroupBindingsBySet: set {} has no bindings — "
                      "declared sets must be contiguous starting at {}",
                      set, base);
        }

        return bindingsBySet;
    }

    vector<Ref<DescriptorSetLayout>>
    ShaderInterface::BuildDescriptorSetLayouts(Context& context, std::string_view namePrefix) const
    {
        const vector<vector<DescriptorBinding>> bindingsBySet = GroupBindingsBySet();
        const u32 setCount = static_cast<u32>(bindingsBySet.size());

        vector<Ref<DescriptorSetLayout>> layouts;
        layouts.reserve(setCount);

        // The layouts are contiguous, prepended after the typed bindless sets, so element i lands
        // at set (FirstUserSet + i); name it by that actual set index.
        for (u32 i = 0; i < setCount; ++i)
        {
            layouts.push_back(DescriptorSetLayout::Create(
                context, DescriptorSetLayoutInfo{
                             .Name = fmt::format("{} Set {}", namePrefix,
                                                 BindlessRegistry::FirstUserSet + i),
                             .Bindings = bindingsBySet[i],
                         }));
        }

        return layouts;
    }
}
