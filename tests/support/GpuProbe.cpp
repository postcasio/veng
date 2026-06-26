#include <support/GpuProbe.h>

// Plain Vulkan C API on purpose: this TU links Vulkan PRIVATE and has nothing
// to do with veng's vulkan.hpp / -fno-exceptions configuration, so the C API
// keeps the probe self-contained and free of that setup.
#include <vector>

#include <vulkan/vulkan.h>

namespace
{
    // Creates a throwaway instance with the portability bits MoltenVK needs.
    // Returns VK_NULL_HANDLE when no loader + ICD is present; the caller owns
    // and destroys the instance on success.
    VkInstance CreateProbeInstance()
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
        {
            return VK_NULL_HANDLE;
        }
        return instance;
    }
}

namespace Veng::Test
{
    bool HasVulkanDriver()
    {
        VkInstance instance = CreateProbeInstance();
        if (instance == VK_NULL_HANDLE)
        {
            return false;
        }
        vkDestroyInstance(instance, nullptr);
        return true;
    }

    bool HasAstcSupport()
    {
        VkInstance instance = CreateProbeInstance();
        if (instance == VK_NULL_HANDLE)
        {
            return false;
        }

        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        std::vector<VkPhysicalDevice> devices(deviceCount);
        if (deviceCount > 0)
        {
            vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        }

        bool supported = false;
        for (VkPhysicalDevice device : devices)
        {
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceFeatures(device, &features);
            if (features.textureCompressionASTC_LDR == VK_TRUE)
            {
                supported = true;
                break;
            }
        }

        vkDestroyInstance(instance, nullptr);
        return supported;
    }
}
