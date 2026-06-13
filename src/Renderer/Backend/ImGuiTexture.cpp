#include <Veng/Renderer/ImGuiTexture.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Backend/Natives.h>

namespace Veng::Renderer
{
    ImGuiTexture::Native& ImGuiTexture::GetNative() const { return *m_Native; }

    ImGuiTexture::ImGuiTexture(Unique<Native> native) : m_Native(std::move(native))
    {

    }

    ImGuiTexture::~ImGuiTexture()
    {
        Context::Instance().DestroyImGuiTexture(*this);
    }

    u64 ImGuiTexture::GetTextureId() const
    {
        return reinterpret_cast<u64>(static_cast<VkDescriptorSet>(m_Native->Set));
    }
}
