#include <Veng/Renderer/Sampler.h>

#include <Veng/Renderer/Context.h>

#include "Natives.h"
#include "TypeMapping.h"

namespace Veng::Renderer
{
    Sampler::Native& Sampler::GetNative() const
    {
        return *m_Native;
    }

    Sampler::Sampler(Context& context, const SamplerInfo& info)
        : m_Context(context), m_Info(info), m_Native(CreateUnique<Native>())
    {
        const ResolvedAnisotropy anisotropy = ResolveAnisotropy(
            info.AnisotropyEnabled, info.MaxAnisotropy, context.GetMaxSamplerAnisotropy());

        MTLSamplerDescriptor* const descriptor = [[MTLSamplerDescriptor alloc] init];
        descriptor.magFilter = Metal::ToMetal(info.MagFilter);
        descriptor.minFilter = Metal::ToMetal(info.MinFilter);
        descriptor.mipFilter = Metal::ToMetal(info.MipmapMode);
        descriptor.sAddressMode = Metal::ToMetal(info.AddressModeU);
        descriptor.tAddressMode = Metal::ToMetal(info.AddressModeV);
        descriptor.rAddressMode = Metal::ToMetal(info.AddressModeW);
        descriptor.borderColor = Metal::ToMetal(info.BorderColor);
        descriptor.lodMinClamp = info.MinLod;
        descriptor.lodMaxClamp = info.MaxLod;
        descriptor.lodBias = info.MipLodBias;
        descriptor.maxAnisotropy = static_cast<NSUInteger>(anisotropy.MaxAnisotropy);
        descriptor.normalizedCoordinates = !info.UnnormalizedCoordinates;
        descriptor.compareFunction =
            info.CompareEnable ? Metal::ToMetal(info.CompareOp) : MTLCompareFunctionNever;
        // Every sampler may be written into an argument buffer, bindless or a set's.
        descriptor.supportArgumentBuffers = YES;
        descriptor.label = Metal::ToNSString(info.Name);

        m_Native->Sampler = [context.GetNative().Device newSamplerStateWithDescriptor:descriptor];
        VE_ASSERT(m_Native->Sampler != nil, "Sampler '{}': Metal refused the sampler", info.Name);
    }

    Sampler::~Sampler()
    {
        m_Context.GetNative().Retire(m_Native->Sampler);
        m_Native->Sampler = nil;
    }
}
