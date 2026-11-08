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
}
