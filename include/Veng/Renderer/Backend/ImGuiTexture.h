#pragma once
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class ImGuiTexture
    {
    public:
        ImGuiTexture(VkDescriptorSet descriptorSet);
        ~ImGuiTexture();

        static Ref<ImGuiTexture> Create(VkDescriptorSet descriptorSet)
        {
            return CreateRef<ImGuiTexture>(descriptorSet);
        }

        [[nodiscard]] VkDescriptorSet GetDescriptorSet() const { return m_DescriptorSet; }

    private:
        VkDescriptorSet m_DescriptorSet;
    };
}
