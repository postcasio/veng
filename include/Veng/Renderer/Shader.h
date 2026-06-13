#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Result.h>

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
        std::span<const u8> Binary;
        string EntryPoint = "main";
    };

    class Shader
    {
    public:
        // Loads SPIR-V from disk. Returns an error Result when the file cannot
        // be opened, so callers can recover (report and continue, retry on
        // hot-reload) instead of terminating. Invalid SPIR-V past that point is
        // fatal via the Vulkan call.
        static Result<Unique<Shader>> Create(const ShaderInfo& info);

        static Unique<Shader> Create(const ShaderBinaryInfo& info)
        {
            return CreateUnique<Shader>(info);
        }

        explicit Shader(const ShaderBinaryInfo& info);
        ~Shader();

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] const string& GetEntryPoint() const { return m_EntryPoint; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        string m_Name;
        string m_EntryPoint;
        Unique<Native> m_Native;
    };
}
