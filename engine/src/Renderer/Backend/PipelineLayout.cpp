#include <Veng/Renderer/PipelineLayout.h>

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

namespace Veng::Renderer
{
    /// @brief Returns the backend-native pipeline layout handle.
    PipelineLayout::Native& PipelineLayout::GetNative() const
    {
        return *m_Native;
    }

    /// @brief Constructs a Vulkan pipeline layout, prepending the typed bindless registry set layouts.
    ///
    /// Sets 0-2 are reserved for the typed bindless registries across every pipeline layout — the
    /// 2D array at 0, the 3D (volume) array at 1, the cube array at 2 — so author-declared sets
    /// shift to BindlessRegistry::FirstUserSet (3). A pipeline sampling only 2D still carries the
    /// 3D and cube set layouts, which costs nothing to bind and lets any material index them.
    /// @param context  The owning render context.
    /// @param info     Layout configuration including descriptor set layouts and push constant ranges.
    PipelineLayout::PipelineLayout(Context& context, const PipelineLayoutInfo& info)
        : m_Context(context), m_Name(info.Name), m_Native(CreateUnique<Native>()),
          m_DescriptorSetLayouts(info.DescriptorSetLayouts),
          m_PushConstantRanges(info.PushConstantRanges)
    {
        const BindlessRegistry& registry = context.GetBindlessRegistry();

        vector<vk::DescriptorSetLayout> descriptorSetLayouts;
        descriptorSetLayouts.reserve(m_DescriptorSetLayouts.size() + BindlessRegistry::SetCount);
        descriptorSetLayouts.push_back(GetVkDescriptorSetLayout(*registry.GetSet0Layout()));
        descriptorSetLayouts.push_back(GetVkDescriptorSetLayout(*registry.GetVolumeSetLayout()));
        descriptorSetLayouts.push_back(GetVkDescriptorSetLayout(*registry.GetCubeSetLayout()));

        for (const auto& descriptorSetLayout : m_DescriptorSetLayouts)
        {
            descriptorSetLayouts.push_back(descriptorSetLayout->GetNative().Layout);
        }

        vector<vk::PushConstantRange> pushConstantRanges;
        pushConstantRanges.reserve(info.PushConstantRanges.size());

        for (const auto& pushConstantRange : info.PushConstantRanges)
        {
            pushConstantRanges.push_back({.stageFlags = ToVk(pushConstantRange.Stages),
                                          .offset = pushConstantRange.Offset,
                                          .size = pushConstantRange.Size});
        }

        const vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo{
            .setLayoutCount = static_cast<u32>(descriptorSetLayouts.size()),
            .pSetLayouts = descriptorSetLayouts.data(),
            .pushConstantRangeCount = static_cast<u32>(pushConstantRanges.size()),
            .pPushConstantRanges = pushConstantRanges.data(),
        };

        m_Native->Layout =
            GetVkDevice(m_Context).createPipelineLayout(pipelineLayoutCreateInfo).value;

        DebugMarkers::MarkPipelineLayout(GetVkDevice(m_Context), m_Native->Layout, m_Name);
    }

    /// @brief Defers destruction of the Vulkan pipeline layout handle until the GPU is done with it.
    PipelineLayout::~PipelineLayout()
    {
        m_Context.GetNative().Retire(m_Native->Layout);
    }
}
