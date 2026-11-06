#include "SphereTerrainLayout.h"

#include <algorithm>

#include <fmt/format.h>

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/PipelineLayout.h>

namespace Veng::Renderer
{
    namespace
    {
        constexpr ShaderStage Stages = ShaderStage::Vertex | ShaderStage::Fragment;

        vector<DescriptorBinding> TerrainBindings()
        {
            vector<DescriptorBinding> bindings;
            for (u32 channel = 0; channel < MaxSphereTerrainChannels; channel++)
            {
                bindings.push_back({.Binding = channel,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = Stages});
            }
            bindings.push_back({.Binding = SphereTerrainRecordsBinding,
                                .Type = DescriptorType::StorageBuffer,
                                .Count = 1,
                                .Stages = Stages});
            bindings.push_back({.Binding = SphereTerrainListBinding,
                                .Type = DescriptorType::StorageBuffer,
                                .Count = 1,
                                .Stages = Stages});
            return bindings;
        }
    }

    bool IsSphereTerrainBinding(const u32 binding, const DescriptorType type)
    {
        return std::ranges::any_of(TerrainBindings(), [&](const DescriptorBinding& own)
                                   { return own.Binding == binding && own.Type == type; });
    }

    Ref<DescriptorSetLayout> CreateSphereTerrainSetLayout(Context& context)
    {
        return DescriptorSetLayout::Create(
            context, {.Name = "Sphere Terrain Set Layout", .Bindings = TerrainBindings()});
    }

    Result<Ref<PipelineLayout>> WithSphereTerrainSet(Context& context,
                                                     const PipelineLayout& reflected, string name)
    {
        vector<Ref<DescriptorSetLayout>> sets = reflected.GetDescriptorSetLayouts();
        const u32 index = SphereTerrainSet - BindlessRegistry::FirstUserSet;
        if (sets.size() <= index || sets[index] == nullptr)
        {
            return std::unexpected(
                string("a sphere-terrain material's stages declare no set 4: its vertex stage is "
                       "not the core terrain stage"));
        }
        for (const DescriptorBinding& binding : sets[index]->GetBindings())
        {
            if (!IsSphereTerrainBinding(binding.Binding, binding.Type))
            {
                return std::unexpected(fmt::format(
                    "a sphere-terrain material declares set 4 binding {}, which the terrain's set "
                    "does not carry",
                    binding.Binding));
            }
        }
        sets[index] = CreateSphereTerrainSetLayout(context);
        return PipelineLayout::Create(context,
                                      {
                                          .Name = std::move(name),
                                          .DescriptorSetLayouts = std::move(sets),
                                          .PushConstantRanges = reflected.GetPushConstantRanges(),
                                      });
    }
}
