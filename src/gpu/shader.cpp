#include "shader.h"
#include "../fs.h"
#include "../engine.h"

Shader::Shader(std::filesystem::path const &path)
{
    auto code = readFile(path);

    shaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderModuleCreateInfo.codeSize = code.size();
    shaderModuleCreateInfo.pCode = reinterpret_cast<const uint32_t *>(code.data());

    VK_CHECK_RESULT(vkCreateShaderModule(renderer()->device, &shaderModuleCreateInfo, nullptr, &shaderModule), "failed to create shader module!");
}

Shader::~Shader()
{
    vkDestroyShaderModule(renderer()->device, shaderModule, nullptr);
}