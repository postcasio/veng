#pragma once

// The Vulkan backend's common header. Native.h carries the vulkan.hpp configuration and the VMA
// include every translation unit parsing Vulkan must agree on; this adds what only the backend
// uses.
#include <Veng/Renderer/Native.h>

#include <vulkan/vulkan_beta.h>
#include <vulkan/vk_enum_string_helper.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#define VK_RAW_ASSERT(f, msg)                                                                      \
    do                                                                                             \
    {                                                                                              \
        const VkResult res = (f);                                                                  \
        if (res != VK_SUCCESS)                                                                     \
        {                                                                                          \
            ::Veng::Detail::FatalAssert(                                                           \
                __FILE__, __LINE__, #f,                                                            \
                ::fmt::format("Vulkan call failed with {}: {}", string_VkResult(res), msg));       \
        }                                                                                          \
    } while (false)

#define VK_ASSERT(f, msg)                                                                          \
    do                                                                                             \
    {                                                                                              \
        const vk::Result res = (f);                                                                \
        if (res != vk::Result::eSuccess)                                                           \
        {                                                                                          \
            ::Veng::Detail::FatalAssert(__FILE__, __LINE__, #f,                                    \
                                        ::fmt::format("Vulkan call failed with {}: {}",            \
                                                      string_VkResult(static_cast<VkResult>(res)), \
                                                      msg));                                       \
        }                                                                                          \
    } while (false)

#define VK_BOOL(b) ((b) ? vk::True : vk::False)
