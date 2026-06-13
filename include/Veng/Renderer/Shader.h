#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Result.h>

namespace Veng::Renderer
{
    class Context;

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
        //
        // Ref, not Unique: shaders are a shared GPU resource (the same module is
        // referenced by PipelineShaderStageInfo on every pipeline that uses it).
        static Result<Ref<Shader>> Create(Context& context, const ShaderInfo& info);

        static Ref<Shader> Create(Context& context, const ShaderBinaryInfo& info)
        {
            return Ref<Shader>(new Shader(context, info));
        }

        ~Shader();

        Shader(const Shader&) = delete;
        Shader& operator=(const Shader&) = delete;

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] const string& GetEntryPoint() const { return m_EntryPoint; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        Shader(Context& context, const ShaderBinaryInfo& info);

        // The context this resource was created with (deferred-destruction
        // back-ref; a resource must not outlive its context).
        Context& m_Context;
        string m_Name;
        string m_EntryPoint;
        Unique<Native> m_Native;
    };
}
