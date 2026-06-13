#include <support/GpuProbe.h>

// Plain Vulkan C API on purpose: this TU links Vulkan PRIVATE and has nothing
// to do with veng's vulkan.hpp / -fno-exceptions configuration, so the C API
// keeps the probe self-contained and free of that setup.
#include <vulkan/vulkan.h>

namespace Veng::Test
{
    bool HasVulkanDriver()
    {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;

#if defined(__APPLE__)
        // MoltenVK is a non-conformant (portability) driver; the loader rejects
        // instance creation on macOS unless the portability-enumeration bit and
        // its extension are set (mirrors Context::Initialize).
        const char* portability = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = &portability;
#endif

        VkInstance instance = VK_NULL_HANDLE;
        if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
            return false;

        vkDestroyInstance(instance, nullptr);
        return true;
    }
}
