#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Scene/Components.h>

namespace Veng
{
    class Scene;
    class AssetManager;
    class Mesh;
    struct MeshData;

    /// @brief Builds the CPU geometry for a primitive shape variant's active alternative.
    ///
    /// Reads the active alternative through the variant ops and calls the matching
    /// Primitives:: generator. Pure CPU — no render Context.
    /// @param shape  The shape recipe; its active alternative selects the generator.
    /// @return The generated MeshData, or nullopt when the variant is empty.
    [[nodiscard]] optional<MeshData> BuildShapeMeshData(const PrimitiveShapeVariant& shape);

    /// @brief Identity of a primitive shape value: its kind, a hash of its parameter bytes,
    ///        and its material — equal keys denote an identical procedural mesh.
    struct ShapeKey
    {
        /// @brief The active alternative's TypeId.
        TypeId Kind = InvalidTypeId;
        /// @brief A hash of the active alternative's parameter bytes.
        u64 ParamHash = 0;
        /// @brief The active alternative's material id.
        AssetId Material;

        /// @brief Equality over all three fields.
        bool operator==(const ShapeKey&) const = default;
    };
}

/// @brief std::hash specialization for ShapeKey, enabling its use as an unordered_map key.
template <>
struct std::hash<Veng::ShapeKey>
{
    /// @brief Combines the kind, parameter hash, and material id into one hash.
    /// @param key  The key to hash.
    /// @return A combined hash of the key's fields.
    Veng::usize operator()(const Veng::ShapeKey& key) const noexcept
    {
        const Veng::usize a = std::hash<Veng::TypeId>{}(key.Kind);
        const Veng::usize b = std::hash<Veng::u64>{}(key.ParamHash);
        const Veng::usize c = std::hash<Veng::AssetId>{}(key.Material);
        Veng::usize h = a;
        h ^= b + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        h ^= c + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

namespace Veng
{
    /// @brief Maps a ShapeKey to the (pending or resident) mesh built for it, so identical
    ///        shapes share one upload and one GPU mesh.
    ///
    /// A plain strong-handle map, touched only on the render thread — no WeakRef, no
    /// CollectGarbage interaction. Owned by the resolution caller (the app or the prefab-editor
    /// document) for its scene's life; dropping a held handle retires the mesh through the
    /// normal per-frame path.
    struct PrimitiveMeshCache
    {
        /// @brief The shape-keyed cache of generated mesh handles.
        unordered_map<ShapeKey, AssetHandle<Mesh>> Entries;
    };

    /// @brief Generates and streams in a Mesh for each PrimitiveComponent whose MeshRenderer
    ///        does not already hold the mesh for its current shape, storing the (pending)
    ///        handle on the entity's MeshRenderer. Adds a MeshRenderer when the entity has none.
    ///
    /// Idempotent: an entity already pointing at its current shape's cached mesh is skipped, so
    /// the app calls it after Prefab::SpawnInto and the prefab editor calls it every frame.
    /// Identical shapes across entities share one cache entry, so a field of identical cubes
    /// uploads once; after the scan, cache entries no entity references are pruned. Render-thread
    /// only — it builds meshes through manager.CreateAsync.
    /// @param scene    The scene whose PrimitiveComponent entities are resolved.
    /// @param manager  The asset manager the async meshes stream through.
    /// @param cache    The caller-owned dedup cache, retained across calls for the scene's life.
    void ResolvePrimitiveMeshes(Scene& scene, AssetManager& manager, PrimitiveMeshCache& cache);
}
