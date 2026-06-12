#include <Veng/Renderer/Backend/Shader.h>

#include <Veng/Renderer/Backend/Context.h>
#include <filesystem>
#include <fstream>

namespace Veng::Renderer
{
    Shader::Shader(const ShaderInfo& info) : m_Name(info.Name), m_EntryPoint(info.EntryPoint)
    {
        path filePath = std::filesystem::absolute(info.Path);
        std::ifstream file(filePath, std::ios::ate | std::ios::binary);

        if (!file.is_open())
        {
            throw std::runtime_error(fmt::format("Can't open file: {}", filePath.string()));
        }

        const u32 fileSize = (u32)file.tellg();
        std::vector<u8> buffer(fileSize);

        file.seekg(0);
        file.read(reinterpret_cast<char*>(buffer.data()), fileSize);

        file.close();

        vk::ShaderModuleCreateInfo createInfo{
            .codeSize = buffer.size(),
            .pCode = reinterpret_cast<const u32*>(buffer.data()),
        };

        m_VkModule = Context::Instance().GetVkDevice().createShaderModule(createInfo);
    }

    Shader::Shader(const ShaderBinaryInfo& info) : m_Name(info.Name), m_EntryPoint(info.EntryPoint)
    {
        vk::ShaderModuleCreateInfo createInfo{
            .codeSize = info.Binary.size_bytes(),
            .pCode = reinterpret_cast<const u32*>(info.Binary.data()),
        };

        m_VkModule = Context::Instance().GetVkDevice().createShaderModule(createInfo);
    }

    Shader::~Shader()
    {
        Context::Instance().GetVkDevice().destroyShaderModule(m_VkModule);
    }
}
