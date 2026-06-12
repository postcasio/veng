#include <Veng/Renderer/Backend/ImGuiTexture.h>

#include <Veng/Renderer/Backend/Context.h>

namespace Veng::Renderer
{
    ImGuiTexture::ImGuiTexture(VkDescriptorSet descriptorSet) : m_DescriptorSet(descriptorSet)
    {

    }

    ImGuiTexture::~ImGuiTexture()
    {
        Context::Instance().DestroyImGuiTexture(*this);
    }
}
