#include <support/GpuProbe.h>

#include <Veng/Renderer/DeviceProbe.h>

namespace Veng::Test
{
    bool HasGpuDevice()
    {
        return Renderer::HasGpuDevice();
    }

    bool HasAstcSupport()
    {
        return Renderer::HasGpuDeviceWithAstc();
    }
}
