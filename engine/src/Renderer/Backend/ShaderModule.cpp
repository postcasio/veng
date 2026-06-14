#include <Veng/Renderer/ShaderModule.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <filesystem>
#include <fstream>

namespace Veng::Renderer
{
    ShaderModule::Native& ShaderModule::GetNative() const { return *m_Native; }

    Result<Ref<ShaderModule>> ShaderModule::Create(Context& context, const ShaderModuleInfo& info)
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

        return Ref<ShaderModule>(new ShaderModule(context, ShaderModuleBinaryInfo{
            .Name = info.Name,
            .Binary = buffer,
            .EntryPoint = info.EntryPoint,
        }));
    }

    ShaderModule::ShaderModule(Context& context, const ShaderModuleBinaryInfo& info) : m_Context(context), m_Name(info.Name), m_EntryPoint(info.EntryPoint),
                                                    m_Native(CreateUnique<Native>())
    {
        vk::ShaderModuleCreateInfo createInfo{
            .codeSize = info.Binary.size_bytes(),
            .pCode = reinterpret_cast<const u32*>(info.Binary.data()),
        };

        m_Native->Module = GetVkDevice(m_Context).createShaderModule(createInfo).value;
    }

    ShaderModule::~ShaderModule()
    {
        m_Context.GetNative().Retire(m_Native->Module);
    }
}
