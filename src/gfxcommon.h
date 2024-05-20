#ifndef _GFXCOMMON_H_
#define _GFXCOMMON_H_

#define GLFW_INCLUDE_VULKAN

#include <GLFW/glfw3.h>

#define GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RIGHT_HANDED
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/string_cast.hpp>

#include <vulkan/vk_enum_string_helper.h>

#include <vk_mem_alloc.h>

#include <iostream>

#include <algorithm>
#include <vector>

#define VK_CHECK_RESULT(f, msg)                                                                                                                      \
    {                                                                                                                                                \
        VkResult res = (f);                                                                                                                          \
        if (res != VK_SUCCESS)                                                                                                                       \
        {                                                                                                                                            \
            std::cout << "Fatal : " << msg << "\nVkResult is \"" << string_VkResult(res) << "\" in " << __FILE__ << " at line " << __LINE__ << "\n"; \
            throw std::runtime_error(msg);                                                                                                           \
        }                                                                                                                                            \
    }

// #define ENABLE_MULTISAMPLING
#endif