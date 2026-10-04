#pragma once

#include <Veng/Cook/CookCache.h>
#include <Veng/Cook/Cooker.h>

#include "TempPath.h"

#include <optional>
#include <span>

namespace Veng::TestSupport
{
    /// @brief The test process's cook cache, opened once under its scratch directory.
    ///
    /// A test that cooks a fixture pack per case re-runs every importer — a Slang compile of every
    /// shader in it — each time, though the pack has not changed between cases. Cooking through this
    /// cache makes every cook after the first a cache hit, and the cooker guarantees the archive is
    /// byte-identical either way, so a test reads exactly what an uncached cook would have written.
    /// The cache lives and dies with the process (TempDir() is per process), so a rebuilt cooker can
    /// never be served a blob an older one cooked, and a fixed tag suffices.
    /// @return The cache, or null when its directory could not be created (the cook then runs
    ///         uncached).
    inline const Cook::CookCache* ProcessCookCache()
    {
        static const std::optional<Cook::CookCache> cache = []() -> std::optional<Cook::CookCache>
        {
            Result<Cook::CookCache> opened =
                Cook::CookCache::Open(TempDir() / "cook-cache", "veng-test-process", "");
            if (!opened.has_value())
            {
                return std::nullopt;
            }
            return std::move(*opened);
        }();
        return cache.has_value() ? &*cache : nullptr;
    }

    /// @brief Cooks a pack exactly as Cooker::CookPack does, through the process's cook cache.
    ///
    /// The parameters are CookPack's, in its order; the cache is the one difference.
    [[nodiscard]] inline VoidResult
    CookCached(const Cook::Cooker& cooker, const path& packJson, const path& outArchive,
               std::span<const path> referencePacks = {}, const TypeRegistry* types = nullptr,
               const SystemRegistry* systems = nullptr, vector<path>* outDependencies = nullptr,
               const BuildConfiguration* config = nullptr, const path& configFile = {},
               const path& shaderIncludeDir = {})
    {
        return cooker.CookPack(packJson, outArchive, referencePacks, types, systems,
                               outDependencies, config, configFile, shaderIncludeDir,
                               ProcessCookCache());
    }
}
