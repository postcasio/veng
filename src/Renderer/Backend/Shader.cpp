#include <Veng/Renderer/Shader.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <filesystem>
#include <fstream>

namespace Veng::Renderer
{
    Shader::Native& Shader::GetNative() const { return *m_Native; }

    Result<Ref<Shader>> Shader::Create(const ShaderInfo& info)
    {
        path filePath = std::filesystem::absolute(info.Path);
        std::ifstream file(filePath, std::ios::ate | std::ios::binary);

        if (!file.is_open())
        {
            return std::unexpected(fmt::format("Can't open shader file: {}", filePath.string()));
        }

        const u32 fileSize = (u32)file.tellg();
        std::vector<u8> buffer(fileSize);

        file.seekg(0);
        file.read(reinterpret_cast<char*>(buffer.data()), fileSize);

        file.close();

        return CreateRef<Shader>(ShaderBinaryInfo{
            .Name = info.Name,
            .Binary = buffer,
            .EntryPoint = info.EntryPoint,
        });
    }

    Shader::Shader(const ShaderBinaryInfo& info) : m_Name(info.Name), m_EntryPoint(info.EntryPoint),
                                                    m_Native(CreateUnique<Native>())
    {
        vk::ShaderModuleCreateInfo createInfo{
            .codeSize = info.Binary.size_bytes(),
            .pCode = reinterpret_cast<const u32*>(info.Binary.data()),
        };

        m_Native->Module = GetVkDevice(Context::Instance()).createShaderModule(createInfo).value;
    }

    Shader::~Shader()
    {
        Context::Instance().GetNative().Retire(m_Native->Module);
    }
}
