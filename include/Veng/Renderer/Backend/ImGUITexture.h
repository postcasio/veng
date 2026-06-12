#pragma once
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class ImGUITexture
    {
    public:
        ImGUITexture(VkDescriptorSet descriptorSet);
        ~ImGUITexture();

        static Ref<ImGUITexture> Create(VkDescriptorSet descriptorSet)
        {
            return CreateRef<ImGUITexture>(descriptorSet);
        }

        [[nodiscard]] VkDescriptorSet GetDescriptorSet() const { return m_DescriptorSet; }

    private:
        VkDescriptorSet m_DescriptorSet;
    };
}
