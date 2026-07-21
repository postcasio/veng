#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/ResidencyBatch.h>
#include <Veng/Reflection/TypeId.h>

#include <span>

namespace Veng
{
    class Scene;
    class AssetManager;

    /// @brief Cached, immutable cooked-prefab asset: a serialized recipe of entities and components.
    ///
    /// Each entity carries components keyed by their stable TypeId; each component's field values
    /// are held as the reflection serializer's name-keyed WriteFields record (no per-component
    /// deserialization at load). Embedded AssetHandle dependencies are resolved as ordinary
    /// load-time dependencies and kept resident for the prefab's lifetime.
    ///
    /// Prefab is CPU data with no GPU resource. Load it through AssetManager::Load like any other
    /// asset, create a Scene, and call SpawnInto.
    class Prefab
    {
    public:
        /// @brief One component on a prefab entity: its TypeId and the WriteFields record bytes.
        struct Component
        {
            /// @brief The component's stable type identity.
            TypeId Type = InvalidTypeId;
            /// @brief Name-keyed reflection record used to populate the component at spawn.
            vector<u8> Record;
        };

        /// @brief One prefab entity and the components it carries, in authored order.
        struct PrefabEntity
        {
            /// @brief The entity's components, in authored order.
            vector<Component> Components;
        };

        /// @brief Options selecting which of a prefab's authored entities a spawn materializes.
        struct SpawnOptions
        {
            /// @brief Skip authored entities that carry Authority::Server (or default to it).
            ///
            /// The client-mode level load: a joining client loads the level's non-authoritative
            /// scaffolding (its own cameras, local viewers, environment) but leaves the
            /// server-authoritative entities out — they arrive from the spawn stream, server-assigned
            /// wire ids and all, so a skipped entity is spawned exactly once. A skipped entity's index
            /// slot resolves to Entity::Null, so a surviving entity's reference to it (or child under
            /// it) reads null until — if ever — the replicated copy binds. Default false: every
            /// authored entity spawns (the standalone/server path).
            bool SkipServerAuthoritative = false;
        };

        /// @brief The product of a spawn: the spawned roots plus the residency batch the spawn introduced.
        struct SpawnResult
        {
            /// @brief The spawned root entities (no parent link), in authoring order.
            vector<Entity> Roots;
            /// @brief The not-yet-resident assets this spawn introduced (recipe-built meshes in practice).
            ResidencyBatch Pending;
        };

        /// @brief Creates a Prefab from a list of entities and their resolved dependency entries.
        ///
        /// @param entities      The prefab's authored entities, in authoring order.
        /// @param dependencies  The resolved embedded-handle cache entries, kept resident.
        /// @param source        The AssetId this prefab loaded from, recorded as each spawned root's
        ///                      PrefabSource. The invalid id (the default) is a runtime-built prefab
        ///                      that no id addresses, and stamps no provenance.
        static Ref<Prefab> Create(vector<PrefabEntity> entities,
                                  vector<Ref<Detail::AssetCacheEntry>> dependencies,
                                  AssetId source = AssetId{});

        /// @brief Spawns this prefab's entities and components into `scene`.
        ///
        /// Returns the spawned roots (those with no parent link — no Hierarchy or a null Hierarchy
        /// parent), in authoring order, plus a ResidencyBatch of the not-yet-resident assets this
        /// spawn introduced. Remaps intra-prefab Entity references to freshly created handles,
        /// rehydrates AssetHandle fields via `manager`, builds inline recipe mesh sources, and
        /// rebuilds the intrusive Hierarchy sibling/child links from each entity's parent edge
        /// (children kept in authored order). Spawning twice produces two independent copies.
        ///
        /// The batch holds the handles this spawn left pending — in practice the recipe-built
        /// meshes (a non-empty MeshRenderer.Source streams in async), since the prefab loader
        /// already asserted every cooked embedded dependency resident before spawn. The caller
        /// owns the batch and decides whether to wait on it (a loading screen, the smoke path)
        /// or let the content stream in over frames.
        /// @param scene    The scene the entities are created in.
        /// @param manager  The asset manager rehydration and recipe builds resolve through.
        /// @return The spawned roots and the residency batch this spawn introduced.
        [[nodiscard]] SpawnResult SpawnInto(Scene& scene, AssetManager& manager) const;

        /// @brief Spawns this prefab, honoring @p options (the client-mode skip of authoritative entities).
        ///
        /// Identical to the two-argument SpawnInto but for the entities @p options excludes: a skipped
        /// entity is never created (its index slot stays Entity::Null so references remap to null), so
        /// a client loading the level in join mode materializes only the non-authoritative scaffolding.
        /// @param scene    The scene the entities are created in.
        /// @param manager  The asset manager rehydration and recipe builds resolve through.
        /// @param options  Which authored entities to materialize.
        /// @return The spawned roots and the residency batch this spawn introduced.
        [[nodiscard]] SpawnResult SpawnInto(Scene& scene, AssetManager& manager,
                                            const SpawnOptions& options) const;

        /// @brief Returns the list of prefab entities.
        [[nodiscard]] const vector<PrefabEntity>& Entities() const { return m_Entities; }

        /// @brief Returns the AssetId this prefab loaded from, or the invalid id if it was built at runtime.
        [[nodiscard]] AssetId GetSourceId() const { return m_SourceId; }

    private:
        Prefab(vector<PrefabEntity> entities, vector<Ref<Detail::AssetCacheEntry>> dependencies,
               AssetId source);

        vector<PrefabEntity> m_Entities;
        /// @brief The AssetId this prefab loaded from; invalid for a runtime-built prefab.
        AssetId m_SourceId;
        /// @brief Resolved embedded-handle cache entries, kept resident so SpawnInto's rehydration is a cheap cache lookup.
        vector<Ref<Detail::AssetCacheEntry>> m_Dependencies;
    };

    /// @brief AssetTypeTrait specialization mapping Prefab to AssetTypes::Prefab.
    template <>
    struct AssetTypeTrait<Prefab>
    {
        /// @brief The asset type tag for Prefab.
        static constexpr AssetTypeId Type = AssetTypes::Prefab;
    };
}
