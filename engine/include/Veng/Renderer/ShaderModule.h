#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Path.h>
#include <Veng/Result.h>

namespace Veng::Renderer
{
    class Context;

    /// @brief The kind of code a ShaderModule is created from.
    ///
    /// A backend accepts only its own kinds: the Vulkan backend takes SPIR-V, and the Metal backend
    /// takes a compiled Metal library or Metal source it builds with the device compiler.
    enum class ShaderCodeKind : u8
    {
        /// @brief A SPIR-V module.
        SpirV,
        /// @brief A compiled Metal library (.metallib).
        MetalLibrary,
        /// @brief Metal Shading Language source text.
        MetalSource,
    };

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

    /// @brief Construction parameters for creating a ShaderModule from in-memory code.
    struct ShaderModuleBinaryInfo
    {
        /// @brief Debug name.
        string Name;
        /// @brief The code, of the kind Kind names; must remain valid for the duration of Create.
        std::span<const u8> Binary;
        /// @brief What Binary holds. Creating a module from a kind the backend compiled in does not
        ///        accept is a fatal error.
        ShaderCodeKind Kind = ShaderCodeKind::SpirV;
        /// @brief Shader entry-point name.
        string EntryPoint = "main";
        /// @brief A compute entry point's threadgroup size, its [numthreads]; zero for any other
        ///        stage.
        ///
        /// A Metal dispatch names its threadgroup size, so a compute pipeline on the Metal backend
        /// requires it; the Vulkan backend reads it from the SPIR-V and ignores this.
        uvec3 ThreadgroupSize{0, 0, 0};
    };

    /// @brief A compiled shader module.
    ///
    /// Shared (Ref) because the same module is referenced by PipelineShaderStageInfo on
    /// every pipeline that uses it.
    class VE_API ShaderModule
    {
    public:
        /// @brief Loads SPIR-V from disk.
        ///
        /// Returns an error Result when the file cannot be opened, so callers can recover
        /// (report and continue, retry on hot-reload) rather than terminate. Invalid SPIR-V
        /// past the file-open point is fatal via the backend call.
        /// @param context  Context for shader-module creation.
        /// @param info     File path and entry-point parameters.
        /// @return A shared reference to the new module, or an error string.
        static Result<Ref<ShaderModule>> Create(Context& context, const ShaderModuleInfo& info);

        /// @brief Creates a ShaderModule from in-memory code.
        /// @param context  Context for shader-module creation.
        /// @param info     Code span, its kind, and entry-point parameters.
        /// @return A shared reference to the new module.
        static Ref<ShaderModule> Create(Context& context, const ShaderModuleBinaryInfo& info)
        {
            return Ref<ShaderModule>(new ShaderModule(context, info));
        }

        /// @brief Defers destruction of the underlying GPU shader module until the GPU is done with it.
        ~ShaderModule();

        ShaderModule(const ShaderModule&) = delete;
        ShaderModule& operator=(const ShaderModule&) = delete;

        /// @brief Returns the debug name supplied at creation.
        [[nodiscard]] const string& GetName() const { return m_Name; }
        /// @brief Returns the shader entry-point name.
        [[nodiscard]] const string& GetEntryPoint() const { return m_EntryPoint; }

        /// @brief A compute entry point's threadgroup size; zero for any other stage.
        [[nodiscard]] uvec3 GetThreadgroupSize() const { return m_ThreadgroupSize; }

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
        /// @brief A compute entry point's threadgroup size.
        uvec3 m_ThreadgroupSize{0, 0, 0};
        /// @brief Backend shader module.
        Unique<Native> m_Native;
    };
}
