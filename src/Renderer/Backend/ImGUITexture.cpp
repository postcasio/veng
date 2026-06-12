#include <Veng/Renderer/Backend/ImGUITexture.h>

#include <Veng/Renderer/Backend/Context.h>

namespace Veng::Renderer
{
    ImGUITexture::ImGUITexture(VkDescriptorSet descriptorSet) : m_DescriptorSet(descriptorSet)
    {

    }

    ImGUITexture::~ImGUITexture()
    {
        Context::Instance().DestroyImGUITexture(*this);
    }
}
