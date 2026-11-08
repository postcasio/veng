#include <Veng/ImGui/ImGuiTexture.h>

#include <Veng/ImGui/ImGuiLayer.h>
#include "ImGuiTextureNative.h"

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
        return PackImGuiTextureId(m_Native->Texture, m_Native->Sampler);
    }
}
