#include <Veng/Renderer/NativeMetal.h>

#include <Veng/Renderer/Context.h>

#include "Natives.h"

namespace Veng::Renderer
{
    void* GetMetalDevice(const Context& context)
    {
        return (__bridge void*)context.GetNative().Device;
    }

    void* GetMetalCommandQueue(const Context& context)
    {
        return (__bridge void*)context.GetNative().Queue;
    }

    void* GetMetalBuffer(const Buffer& buffer)
    {
        return (__bridge void*)buffer.GetNative().Buffer;
    }

    void* GetMetalTexture(const Image& image)
    {
        return (__bridge void*)image.GetNative().Texture;
    }

    void* GetMetalTexture(const ImageView& view)
    {
        return (__bridge void*)view.GetNative().Texture;
    }

    void* GetMetalSamplerState(const Sampler& sampler)
    {
        return (__bridge void*)sampler.GetNative().Sampler;
    }

    void* GetMetalLibrary(const ShaderModule& module)
    {
        return (__bridge void*)module.GetNative().Library;
    }

    void* GetMetalComputePipelineState(const ComputePipeline& pipeline)
    {
        return (__bridge void*)pipeline.GetNative().State;
    }

    void* GetMetalCommandBuffer(const CommandBuffer& commandBuffer)
    {
        return (__bridge void*)commandBuffer.GetNative().CommandBuffer;
    }

    void* GetMetalSharedEvent(const Fence& fence)
    {
        return (__bridge void*)fence.GetNative().Event;
    }

    void* GetMetalSharedEvent(const TimelineSemaphore& timeline)
    {
        return (__bridge void*)timeline.GetNative().Event;
    }
}
