#include <Veng/Renderer/Sampler.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

namespace Veng::Renderer
{
    Sampler::Native& Sampler::GetNative() const { return *m_Native; }

    Sampler::Sampler(const SamplerInfo& info) : m_Context(Context::Instance()), m_Name(info.Name), m_Native(CreateUnique<Native>())
    {
        const vk::SamplerCreateInfo samplerCreateInfo{
            .magFilter = ToVk(info.MagFilter),
            .minFilter = ToVk(info.MinFilter),
            .mipmapMode = ToVk(info.MipmapMode),
            .addressModeU = ToVk(info.AddressModeU),
            .addressModeV = ToVk(info.AddressModeV),
            .addressModeW = ToVk(info.AddressModeW),
            .mipLodBias = info.MipLodBias,
            .anisotropyEnable = info.AnisotropyEnabled,
            .maxAnisotropy = info.MaxAnisotropy,
            .compareEnable = info.CompareEnable,
            .compareOp = ToVk(info.CompareOp),
            .minLod = info.MinLod,
            .maxLod = info.MaxLod,
            .borderColor = ToVk(info.BorderColor),
            .unnormalizedCoordinates = info.UnnormalizedCoordinates,
        };

        m_Native->Sampler = GetVkDevice(m_Context).createSampler(samplerCreateInfo).value;

        DebugMarkers::MarkSampler(m_Native->Sampler, m_Name);
    }

    Sampler::~Sampler()
    {
        m_Context.GetNative().Retire(m_Native->Sampler);
    }
}
