#include <Veng/ImGui/ImGuiTexture.h>

#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Renderer/Backend/Natives.h>

namespace Veng
{
    ImGuiTexture::Native& ImGuiTexture::GetNative() const
    {
        return *m_Native;
    }

    ImGuiTexture::ImGuiTexture(Unique<Native> native, ImGuiLayer& layer)
        : m_Native(std::move(native)), m_Layer(layer)
    {
    }

    ImGuiTexture::~ImGuiTexture()
    {
        m_Layer.DestroyTexture(*this);
    }

    u64 ImGuiTexture::GetTextureId() const
    {
        return reinterpret_cast<u64>(static_cast<VkDescriptorSet>(m_Native->Set));
    }
}
