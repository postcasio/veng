#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Localization/LocaleCatalog.h>

namespace Veng
{
    /// @brief AssetTypes::LocaleCatalog loader: decodes a cooked catalog blob into a LocaleCatalog.
    ///
    /// CPU-only, no dependencies, no finalize (the DataTable/Font loader shape without the streamed
    /// dependency): it validates the header, reads the sorted key table and string pool, and builds
    /// each entry's decoded message. A blob whose magic or version does not match, or whose spans
    /// run past the pool, is rejected as Corrupt.
    class LocaleCatalogLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::LocaleCatalog.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::LocaleCatalog; }

        /// @brief Decodes the cooked catalog blob into a LoadJob producing a resident LocaleCatalog.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
