#pragma once

// Cooks a cooker-suite fixture pack the way a project's pack cook runs: with the engine's shader
// headers on the include path, which every fixture shader declaring its resources through
// Veng/binding.slang needs.

#include <span>

#include <Veng/Cook/Cooker.h>

namespace Veng::Test
{
    /// @brief Cooks @p packJson to @p outArchive with the engine shader directory on the include
    ///        path.
    /// @param cooker         The cooker, its importers registered.
    /// @param packJson       The fixture pack.
    /// @param outArchive     Where the archive is written.
    /// @param referencePacks Packs the cook resolves cross-pack ids against.
    /// @return The cook's result.
    [[nodiscard]] inline VoidResult CookFixturePack(const Cook::Cooker& cooker,
                                                    const path& packJson, const path& outArchive,
                                                    const std::span<const path> referencePacks = {})
    {
        return cooker.CookPack(packJson, outArchive, referencePacks, nullptr, nullptr, nullptr,
                               nullptr, {}, path(VENG_CORE_SHADER_DIR));
    }
}
