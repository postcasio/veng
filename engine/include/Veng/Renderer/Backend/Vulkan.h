#pragma once

#include <Veng/Assert.h>

// veng raises no exceptions and builds with -fno-exceptions, so vulkan.hpp's
// throwing convenience overloads must be switched off. With NO_EXCEPTIONS the
// value-returning calls (device.createX(...)) return vk::ResultValue<T> — read
// the result via `.value` — and vulkan.hpp's internal result checks route to
// VULKAN_HPP_ASSERT_ON_RESULT, which we point at the fatal-assert path below.
// Both macros must be defined before <vulkan/vulkan.hpp> is first included.
#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_ASSERT_ON_RESULT(expr)                                                          \
    VE_ASSERT((expr), "vulkan.hpp call returned a non-success vk::Result")

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_NO_CONSTRUCTORS
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_beta.h>
#include <vulkan/vk_enum_string_helper.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <Veng/Renderer/Backend/VulkanMemoryAllocator.h>

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
