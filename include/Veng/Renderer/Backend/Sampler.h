#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    struct SamplerInfo
    {
        string Name;
        vk::SamplerCreateFlags Flags{};
        vk::Filter MagFilter = vk::Filter::eLinear;
        vk::Filter MinFilter = vk::Filter::eLinear;
        vk::SamplerMipmapMode MipmapMode = vk::SamplerMipmapMode::eLinear;
        vk::SamplerAddressMode AddressModeU = vk::SamplerAddressMode::eRepeat;
        vk::SamplerAddressMode AddressModeV = vk::SamplerAddressMode::eRepeat;
        vk::SamplerAddressMode AddressModeW = vk::SamplerAddressMode::eRepeat;
        f32 MipLodBias = 0;
        bool AnisotropyEnabled = true;
        f32 MaxAnisotropy = 8;
        bool CompareEnable = false;
        vk::CompareOp CompareOp = vk::CompareOp::eAlways;
        f32 MinLod = 0;
        f32 MaxLod = 1;
        vk::BorderColor BorderColor = vk::BorderColor::eIntOpaqueBlack;
        bool UnnormalizedCoordinates = false;
    };

    class Sampler
    {
    public:
        static Ref<Sampler> Create(const SamplerInfo& info)
        {
            return CreateRef<Sampler>(info);
        }

        explicit Sampler(const SamplerInfo& info);
        ~Sampler();

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] vk::Sampler GetVkSampler() const { return m_VkSampler; }

    private:
        string m_Name;
        vk::Sampler m_VkSampler;
    };
}
