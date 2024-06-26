#include "shader.h"
#include "../fs.h"
#include "../engine.h"

Shader::Shader(LogicalDevice &device, std::filesystem::path const &path) : device(device)
{
    auto code = readFile(path);

    shaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderModuleCreateInfo.codeSize = code.size();
    shaderModuleCreateInfo.pCode = reinterpret_cast<const uint32_t *>(code.data());

    VK_CHECK_RESULT(vkCreateShaderModule(device.device, &shaderModuleCreateInfo, nullptr, &shaderModule), "failed to create shader module!");
}

Shader::~Shader()
{
    device.destroyShader(shaderModule);
}