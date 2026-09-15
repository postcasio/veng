#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Localization/LocaleIndex.h>

namespace Veng
{
    /// @brief AssetTypes::LocaleIndex loader: decodes a cooked index blob into a LocaleIndex.
    ///
    /// CPU-only, no dependencies, no finalize: it validates the header, reads the locale table and
    /// the endonym pool, and builds the runtime index. It never loads the catalogs it names — the
    /// localization service resolves those by id. A blob whose magic or version does not match, or
    /// whose spans run past the pool, is rejected as Corrupt.
    class LocaleIndexLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::LocaleIndex.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::LocaleIndex; }

        /// @brief Decodes the cooked index blob into a LoadJob producing a resident LocaleIndex.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
