#pragma once

#include <Veng/Asset/Path.h>
#include <Veng/Asset/Types.h>
#include <Veng/Cook/Export.h>

#include <memory>
#include <optional>

namespace Veng::Cook
{
    /// @brief What to compile for the host: a pure Slang module and the C++ namespace it lands in.
    struct HostShaderModuleInfo
    {
        /// @brief The `.slang` module to compile; its directory leads the Slang search path.
        path Module;
        /// @brief The C++ identifier naming the generated namespace, header and source.
        string Name;
        /// @brief The engine core shader dir, second on the search path; empty for none.
        path ShaderIncludeDir;
    };

    /// @brief The generated C++ for one host module, before it is written to disk.
    struct HostShaderModuleOutput
    {
        /// @brief `<Name>.cpp`: the module's C++ in its own namespace, plus the glm-typed wrappers.
        string Source;
        /// @brief `<Name>.h`: one declaration per exported function in `namespace <Name>`.
        string Header;
        /// @brief Every file the module pulled in, the module itself included.
        vector<path> Dependencies;
    };

    /// @brief Compiles pure Slang modules to C++ for evaluation on the host.
    ///
    /// The module loads through the same session policy the shader cook uses (the source dir,
    /// then the engine core shader dir, column-major matrices), so a file that `#include`s
    /// `Veng/...` resolves identically for the GPU and for the host. Slang's `cpp` output is
    /// wrapped in a namespace private to the module, with the prelude's types in a second one,
    /// so several host modules link into one binary without colliding.
    ///
    /// A host module must be pure: it declares no global shader parameter and no entry point,
    /// and each `export __extern_cpp` function takes and returns only `float`, `double`, `int`
    /// and `uint` scalars and their 2- to 4-component vectors, returning anything more through
    /// `out` or `inout` parameters. Anything else is refused with an error naming it.
    ///
    /// The compiler holds one Slang global session, which is costly to create, so a caller
    /// compiling several modules keeps one compiler. It is not thread-safe.
    class VE_COOK_API HostShaderCompiler
    {
    public:
        /// @brief Creates a compiler, or reports why the Slang global session could not be made.
        /// @return The compiler, or an error.
        [[nodiscard]] static Result<HostShaderCompiler> Create();

        /// @brief Destroys the compiler and its Slang session.
        ~HostShaderCompiler();
        /// @brief Moves a compiler, leaving the source empty.
        HostShaderCompiler(HostShaderCompiler&&) noexcept;
        /// @brief Move-assigns a compiler, leaving the source empty.
        HostShaderCompiler& operator=(HostShaderCompiler&&) noexcept;
        HostShaderCompiler(const HostShaderCompiler&) = delete;
        HostShaderCompiler& operator=(const HostShaderCompiler&) = delete;

        /// @brief Compiles one module to C++.
        ///
        /// Errors name the module, and the function, parameter or global that broke the rules.
        /// @param info  The module, its C++ name and the engine shader include dir.
        /// @return The generated source, header and dependency list, or a located error.
        [[nodiscard]] Result<HostShaderModuleOutput> Compile(const HostShaderModuleInfo& info);

        /// @brief The build tag of the Slang the compiler runs on (e.g. `2026.18.3-2-g...`).
        [[nodiscard]] string GetSlangVersion() const;

    private:
        struct Impl;
        explicit HostShaderCompiler(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> m_Impl;
    };

    /// @brief Writes `<Name>.cpp` and `<Name>.h` into a directory, and optionally a depfile.
    ///
    /// Each file is written atomically. The depfile names `<Name>.cpp` as its target and every
    /// dependency the compile recorded.
    /// @param output   The compiled module.
    /// @param name     The module's C++ name, which names the files.
    /// @param outDir   The directory to write into; created when missing.
    /// @param depfile  The depfile to write, or nullopt for none.
    /// @return Success, or a located error.
    [[nodiscard]] VE_COOK_API VoidResult WriteHostShaderModule(const HostShaderModuleOutput& output,
                                                               const string& name,
                                                               const path& outDir,
                                                               const std::optional<path>& depfile);
}
