#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Result.h>

namespace Veng::Renderer
{
    class Context;

    /// @brief Construction parameters for loading a ShaderModule from a SPIR-V file on disk.
    struct ShaderModuleInfo
    {
        /// @brief Debug name.
        string Name;
        /// @brief Path to the SPIR-V binary on disk.
        path Path;
        /// @brief Shader entry-point name.
        string EntryPoint = "main";
    };

    /// @brief Construction parameters for creating a ShaderModule from an in-memory SPIR-V binary.
    struct ShaderModuleBinaryInfo
    {
        /// @brief Debug name.
        string Name;
        /// @brief SPIR-V binary data; must remain valid for the duration of Create.
        std::span<const u8> Binary;
        /// @brief Shader entry-point name.
        string EntryPoint = "main";
    };

    /// @brief A compiled SPIR-V shader module.
    ///
    /// Shared (Ref) because the same module is referenced by PipelineShaderStageInfo on
    /// every pipeline that uses it.
    class ShaderModule
    {
    public:
        /// @brief Loads SPIR-V from disk.
        ///
        /// Returns an error Result when the file cannot be opened, so callers can recover
        /// (report and continue, retry on hot-reload) rather than terminate. Invalid SPIR-V
        /// past the file-open point is fatal via the Vulkan call.
        /// @param context  Context for Vulkan shader-module creation.
        /// @param info     File path and entry-point parameters.
        /// @return A shared reference to the new module, or an error string.
        static Result<Ref<ShaderModule>> Create(Context& context, const ShaderModuleInfo& info);

        /// @brief Creates a ShaderModule from an in-memory SPIR-V binary.
        /// @param context  Context for Vulkan shader-module creation.
        /// @param info     Binary span and entry-point parameters.
        /// @return A shared reference to the new module.
        static Ref<ShaderModule> Create(Context& context, const ShaderModuleBinaryInfo& info)
        {
            return Ref<ShaderModule>(new ShaderModule(context, info));
        }

        /// @brief Defers destruction of the underlying Vulkan shader module until the GPU is done with it.
        ~ShaderModule();

        ShaderModule(const ShaderModule&) = delete;
        ShaderModule& operator=(const ShaderModule&) = delete;

        /// @brief Returns the debug name supplied at creation.
        [[nodiscard]] const string& GetName() const { return m_Name; }
        /// @brief Returns the shader entry-point name.
        [[nodiscard]] const string& GetEntryPoint() const { return m_EntryPoint; }

        /// @brief Opaque backend handle; defined in ShaderModule.cpp.
        struct Native;
        /// @brief Returns the backend handle. Mutable ref from a const method by design — see Native.h.
        [[nodiscard]] Native& GetNative() const;

    private:
        ShaderModule(Context& context, const ShaderModuleBinaryInfo& info);

        /// @brief Context this resource was created with; must outlive the module.
        Context& m_Context;
        /// @brief Debug name.
        string m_Name;
        /// @brief Entry-point name.
        string m_EntryPoint;
        /// @brief Backend Vulkan shader module.
        Unique<Native> m_Native;
    };
}
