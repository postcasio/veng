#pragma once

#include <string_view>

#include <Veng/Asset/Types.h>

namespace Veng::Cook
{
    /// @brief Compiles Metal Shading Language source to a Metal library with the host toolchain.
    ///
    /// Runs `xcrun metal` (Metal 4, fast math) and then `xcrun metallib` in a private temporary
    /// directory, which is removed afterwards.
    /// @param source The MSL text.
    /// @param label  Names the shader in the error a failed compile returns.
    /// @return The .metallib bytes, or the compiler's diagnostics.
    [[nodiscard]] Result<vector<u8>> CompileMetalLibrary(std::string_view source,
                                                         std::string_view label);

    /// @brief The name of the entry-point function in Slang's Metal output.
    ///
    /// Slang renames an entry point whose name Metal reserves (`main` becomes `main_0`), so the
    /// pipeline must use the name the generated source declares.
    /// @param source The MSL text Slang generated for one entry point.
    /// @return The function name, or nullopt when the source declares no stage function.
    [[nodiscard]] optional<string> FindMetalEntryPoint(std::string_view source);
}
