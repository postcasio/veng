#include <Veng/Renderer/ShaderInterface.h>

#include <Veng/Assert.h>
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
                return binding;
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

    vector<Ref<DescriptorSetLayout>> ShaderInterface::BuildDescriptorSetLayouts(Context& context, std::string_view namePrefix) const
    {
        if (Bindings.empty())
            return {};

        u32 maxSet = 0;
        for (const ShaderBinding& binding : Bindings)
        {
            VE_ASSERT(binding.Set >= 1,
                "ShaderInterface::BuildDescriptorSetLayouts: binding '{}' targets set {} — set 0 is reserved for the bindless registry",
                binding.Name, binding.Set);
            maxSet = std::max(maxSet, binding.Set);
        }

        // bindingsBySet[i] holds the bindings declared for set (i + 1).
        vector<vector<DescriptorBinding>> bindingsBySet(maxSet);
        for (const ShaderBinding& binding : Bindings)
        {
            bindingsBySet[binding.Set - 1].push_back(DescriptorBinding{
                .Binding = binding.Binding,
                .Type = binding.Type,
                .Count = binding.Count,
                .Stages = binding.Stages,
            });
        }

        vector<Ref<DescriptorSetLayout>> layouts;
        layouts.reserve(maxSet);

        for (u32 set = 1; set <= maxSet; ++set)
        {
            VE_ASSERT(!bindingsBySet[set - 1].empty(),
                "ShaderInterface::BuildDescriptorSetLayouts: set {} has no bindings — declared sets must be contiguous starting at 1",
                set);

            layouts.push_back(DescriptorSetLayout::Create(context, DescriptorSetLayoutInfo{
                .Name = fmt::format("{} Set {}", namePrefix, set),
                .Bindings = bindingsBySet[set - 1],
            }));
        }

        return layouts;
    }

}
