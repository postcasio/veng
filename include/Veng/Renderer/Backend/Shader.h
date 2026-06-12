#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    struct ShaderInfo
    {
        string Name;
        path Path;
        string EntryPoint = "main";
    };

    struct ShaderBinaryInfo
    {
        string Name;
        std::span<u8> Binary;
        string EntryPoint = "main";
    };

    class Shader
    {
    public:
        static Unique<Shader> Create(const ShaderInfo& info)
        {
            return CreateUnique<Shader>(info);
        }

        static Unique<Shader> Create(const ShaderBinaryInfo& info)
        {
            return CreateUnique<Shader>(info);
        }

        explicit Shader(const ShaderInfo& info);
        explicit Shader(const ShaderBinaryInfo& info);
        ~Shader();

        [[nodiscard]] vk::ShaderModule GetVkModule() const { return m_VkModule; }
        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] const string& GetEntryPoint() const { return m_EntryPoint; }

    private:
        string m_Name;
        string m_EntryPoint;
        vk::ShaderModule m_VkModule;
    };
}
