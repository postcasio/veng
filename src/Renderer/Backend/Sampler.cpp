#include <Veng/Renderer/Backend/Sampler.h>

#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>

namespace Veng::Renderer
{
    Sampler::Sampler(const SamplerInfo& info) : m_Name(info.Name)
    {
        const vk::SamplerCreateInfo samplerCreateInfo{
            .flags = info.Flags,
            .magFilter = info.MagFilter,
            .minFilter = info.MinFilter,
            .mipmapMode = info.MipmapMode,
            .addressModeU = info.AddressModeU,
            .addressModeV = info.AddressModeV,
            .addressModeW = info.AddressModeW,
            .mipLodBias = info.MipLodBias,
            .anisotropyEnable = info.AnisotropyEnabled,
            .maxAnisotropy = info.MaxAnisotropy,
            .compareEnable = info.CompareEnable,
            .compareOp = info.CompareOp,
            .minLod = info.MinLod,
            .maxLod = info.MaxLod,
            .borderColor = info.BorderColor,
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
