#ifndef _GPU_SHADER_H_
#define _GPU_SHADER_H_

#include "../gfxcommon.h"

#include <filesystem>

class Shader
{
public:
    Shader(std::filesystem::path const &path);
    ~Shader();

    VkShaderModuleCreateInfo shaderModuleCreateInfo{};
    VkShaderModule shaderModule;
};

#endif