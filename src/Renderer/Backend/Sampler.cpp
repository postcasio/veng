#include <Veng/Renderer/Backend/Sampler.h>

#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

namespace Veng::Renderer
{
    Sampler::Sampler(const SamplerInfo& info) : m_Name(info.Name)
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

        m_VkSampler = Context::Instance().GetVkDevice().createSampler(samplerCreateInfo).value;

        DebugMarkers::MarkSampler(m_VkSampler, m_Name);
    }

    Sampler::~Sampler()
    {
        Context::Instance().Retire(m_VkSampler);
    }
}
