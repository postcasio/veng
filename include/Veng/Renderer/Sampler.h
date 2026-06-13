#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;

    struct SamplerInfo
    {
        string Name;
        Filter MagFilter = Filter::Linear;
        Filter MinFilter = Filter::Linear;
        MipmapMode MipmapMode = MipmapMode::Linear;
        AddressMode AddressModeU = AddressMode::Repeat;
        AddressMode AddressModeV = AddressMode::Repeat;
        AddressMode AddressModeW = AddressMode::Repeat;
        f32 MipLodBias = 0;
        bool AnisotropyEnabled = true;
        f32 MaxAnisotropy = 8;
        bool CompareEnable = false;
        CompareOp CompareOp = CompareOp::Always;
        f32 MinLod = 0;
        f32 MaxLod = 1;
        BorderColor BorderColor = BorderColor::OpaqueBlack;
        bool UnnormalizedCoordinates = false;
    };

    class Sampler
    {
    public:
        static Ref<Sampler> Create(Context& context, const SamplerInfo& info)
        {
            return Ref<Sampler>(new Sampler(context, info));
        }

        ~Sampler();

        Sampler(const Sampler&) = delete;
        Sampler& operator=(const Sampler&) = delete;

        [[nodiscard]] const string& GetName() const { return m_Name; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        Sampler(Context& context, const SamplerInfo& info);

        // The context this resource was created with (deferred-destruction
        // back-ref; a resource must not outlive its context).
        Context& m_Context;
        string m_Name;
        Unique<Native> m_Native;
    };
}
