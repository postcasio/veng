#include <Veng/Renderer/DeviceProbe.h>

#import <Metal/Metal.h>

namespace Veng::Renderer
{
    bool HasGpuDevice()
    {
        const id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        return device != nil && [device supportsFamily:MTLGPUFamilyMetal4];
    }

    bool HasGpuDeviceWithAstc()
    {
        const id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        return device != nil && [device supportsFamily:MTLGPUFamilyMetal4] &&
               [device supportsFamily:MTLGPUFamilyApple2];
    }
}
