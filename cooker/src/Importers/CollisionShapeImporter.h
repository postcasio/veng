#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.collision.json source into a CookedCollisionShapeHeader plus geometry.
    ///
    /// The source JSON's "model" path (relative to the source JSON's directory) names the model
    /// the geometry is read from, and its "mode" selects `"convex"` (the model's convex hull,
    /// computed here so the runtime builds its shape over a small point set) or `"mesh"` (the
    /// model's welded triangles, for geometry no solver integrates). assimp is a cooker-only
    /// dependency; the hull is this library's own, so the offline toolchain links no solver.
    ///
    /// The blob carries neutral geometry rather than a solver's serialized shape: a solver
    /// version bump is then a rebuild rather than a re-cook of every pack, and the archive format
    /// stays free of any third-party layout.
    class CollisionShapeImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::CollisionShape.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::CollisionShape; }

        /// @brief Returns ImporterConcurrency::Serialized — the geometry is read through assimp.
        ///
        /// The hulling and welding are pure CPU over the importer's own buffers, but the model
        /// read in front of them is not: assimp's DefaultLogger singleton and per-format loader
        /// state are what put every model-reading importer in the serialized band, and an
        /// importer that cannot be shown safe is never assumed safe.
        [[nodiscard]] ImporterConcurrency Concurrency() const override
        {
            return ImporterConcurrency::Serialized;
        }

        /// @brief Cooks the collision shape described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
