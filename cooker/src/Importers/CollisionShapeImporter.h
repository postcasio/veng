#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.collision.json source into a CookedCollisionShapeHeader plus geometry.
    ///
    /// The source JSON's "mode" selects one of three shapes:
    /// - `"convex"` reduces the "model" to its convex hull (computed here so the runtime builds its
    ///   shape over a small point set),
    /// - `"mesh"` welds the "model"'s triangles into an indexed soup, for geometry no solver
    ///   integrates, and
    /// - `"compound"` builds a set of transformed "children", each a primitive (box/sphere/capsule,
    ///   described inline) or its own convex/mesh (naming a model), placed by a per-child "offset"
    ///   and "rotation".
    ///
    /// A "model" path is relative to the source JSON's directory. assimp is a cooker-only
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
