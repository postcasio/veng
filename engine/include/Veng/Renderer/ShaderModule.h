#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Result.h>

namespace Veng::Renderer
{
    class Context;

    struct ShaderModuleInfo
    {
        string Name;
        path Path;
        string EntryPoint = "main";
    };

    struct ShaderModuleBinaryInfo
    {
        string Name;
        std::span<const u8> Binary;
        string EntryPoint = "main";
    };

    class ShaderModule
    {
    public:
        // Loads SPIR-V from disk. Returns an error Result when the file cannot
        // be opened, so callers can recover (report and continue, retry on
        // hot-reload) instead of terminating. Invalid SPIR-V past that point is
        // fatal via the Vulkan call.
        //
        // Ref, not Unique: shaders are a shared GPU resource (the same module is
        // referenced by PipelineShaderStageInfo on every pipeline that uses it).
        static Result<Ref<ShaderModule>> Create(Context& context, const ShaderModuleInfo& info);

        static Ref<ShaderModule> Create(Context& context, const ShaderModuleBinaryInfo& info)
        {
            return Ref<ShaderModule>(new ShaderModule(context, info));
        }

        ~ShaderModule();

        ShaderModule(const ShaderModule&) = delete;
        ShaderModule& operator=(const ShaderModule&) = delete;

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] const string& GetEntryPoint() const { return m_EntryPoint; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        ShaderModule(Context& context, const ShaderModuleBinaryInfo& info);

        // The context this resource was created with (deferred-destruction
        // back-ref; a resource must not outlive its context).
        Context& m_Context;
        string m_Name;
        string m_EntryPoint;
        Unique<Native> m_Native;
    };
}
