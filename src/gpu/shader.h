#ifndef _GPU_SHADER_H_
#define _GPU_SHADER_H_

#include "../gfxcommon.h"

#include "logical_device.h"

#include <filesystem>

class Shader
{
public:
    Shader(LogicalDevice &device, std::filesystem::path const &path);
    ~Shader();

    VkShaderModuleCreateInfo shaderModuleCreateInfo{};
    VkShaderModule shaderModule;

    LogicalDevice &device;
};

#endif