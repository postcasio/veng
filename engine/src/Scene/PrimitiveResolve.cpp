#include <Veng/Scene/PrimitiveResolve.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>

#include <cstring>
#include <unordered_set>

namespace Veng
{
    namespace
    {
        /// @brief TypeId of the AssetHandle<Material> leaf, used to find a shape's material field.
        constexpr TypeId MaterialHandleTypeId = TypeIdOf<AssetHandle<Material>>();

        /// @brief Folds a byte block into a running FNV-1a hash.
        u64 HashBytes(u64 seed, const void* data, usize size)
        {
            const auto* bytes = static_cast<const u8*>(data);
            u64 hash = seed;
            for (usize i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= 0x100000001B3ULL;
            }
            return hash;
        }

        /// @brief The material id recorded on a shape value, or the invalid id when it has none.
        ///
        /// Reads the AssetId off the AssetHandle<Material> field's leading bytes (offset 0 of
        /// the handle) without naming the shape's concrete type.
        AssetId ShapeMaterial(const void* member, const TypeInfo& info)
        {
            for (const FieldDescriptor& field : info.Fields)
            {
                if (field.Class == FieldClass::AssetHandle && field.Type == MaterialHandleTypeId)
                {
                    AssetId id;
                    std::memcpy(&id, static_cast<const u8*>(member) + field.Offset, sizeof(id));
                    return id;
                }
            }
            return AssetId{};
        }

        /// @brief Hashes a shape's numeric parameters, skipping its material handle.
        ///
        /// The material is keyed separately, and an AssetHandle carries a Ref pointer that is
        /// not stable across identical recipes — hashing the parameter bytes minus the handle
        /// keeps the key value-based. Per-field byte length comes from each field type's
        /// registered TypeInfo.
        u64 ShapeParamHash(const void* member, const TypeInfo& info, const TypeRegistry& types)
        {
            u64 hash = 0xCBF29CE484222325ULL;
            for (const FieldDescriptor& field : info.Fields)
            {
                if (field.Class == FieldClass::AssetHandle)
                {
                    continue;
                }
                const usize size = types.Info(field.Type).Size;
                hash = HashBytes(hash, static_cast<const u8*>(member) + field.Offset, size);
            }
            return hash;
        }

        /// @brief Builds the ShapeKey for a variant's active member, or nullopt when empty.
        optional<ShapeKey> KeyFor(const PrimitiveShapeVariant& shape, const TypeRegistry& types)
        {
            const TypeId kind = shape.ActiveType();
            if (kind == InvalidTypeId)
            {
                return std::nullopt;
            }

            const void* member = shape.ActivePtr();
            const TypeInfo& info = types.Info(kind);
            return ShapeKey{
                .Kind = kind,
                .ParamHash = ShapeParamHash(member, info, types),
                .Material = ShapeMaterial(member, info),
            };
        }

        /// @brief A debug name for a generated primitive mesh, keyed by its kind and material.
        string ShapeName(const ShapeKey& key)
        {
            return fmt::format("Primitive {:#018x}/{:#018x}", key.Kind, key.Material.Value);
        }
    }

    optional<MeshData> BuildShapeMeshData(const PrimitiveShapeVariant& shape)
    {
        const TypeId kind = shape.ActiveType();
        const void* member = shape.ActivePtr();
        if (kind == InvalidTypeId || member == nullptr)
        {
            return std::nullopt;
        }

        if (kind == TypeIdOf<CubeShape>())
        {
            const auto& cube = *static_cast<const CubeShape*>(member);
            return Primitives::Cube(cube.Extent, cube.Material);
        }
        if (kind == TypeIdOf<PlaneShape>())
        {
            const auto& plane = *static_cast<const PlaneShape*>(member);
            return Primitives::Plane(plane.Size, plane.Subdivisions, plane.Material);
        }
        if (kind == TypeIdOf<SphereShape>())
        {
            const auto& sphere = *static_cast<const SphereShape*>(member);
            return Primitives::Sphere(sphere.Radius, sphere.Rings, sphere.Segments,
                                      sphere.Material);
        }
        if (kind == TypeIdOf<IcosphereShape>())
        {
            const auto& ico = *static_cast<const IcosphereShape*>(member);
            return Primitives::Icosphere(ico.Radius, ico.Subdivisions, ico.Material);
        }

        return std::nullopt;
    }

    void ResolvePrimitiveMeshes(Scene& scene, AssetManager& manager, PrimitiveMeshCache& cache)
    {
        const TypeRegistry& types = manager.GetTypeRegistry();

        // Collect the entities first: resolving may Add a MeshRenderer, and a structural
        // change mid-Each is illegal.
        vector<Entity> entities;
        scene.Each<PrimitiveComponent>([&](Entity entity, PrimitiveComponent&)
                                       { entities.push_back(entity); });

        for (const Entity entity : entities)
        {
            const PrimitiveComponent& primitive = scene.Get<PrimitiveComponent>(entity);
            const optional<ShapeKey> key = KeyFor(primitive.Shape, types);
            if (!key)
            {
                continue; // empty variant — no mesh
            }

            const MeshRenderer* renderer = scene.TryGet<MeshRenderer>(entity);

            // Already resolved to this shape: the renderer holds the very cache entry the
            // current key maps to. Editing the shape changes the key, so the stored handle
            // no longer matches and the entity re-resolves below.
            const auto cached = cache.Entries.find(*key);
            if (renderer != nullptr && cached != cache.Entries.end() &&
                AssetManager::EntryOf(renderer->Mesh) == AssetManager::EntryOf(cached->second))
            {
                continue;
            }

            AssetHandle<Mesh> handle;
            if (cached != cache.Entries.end())
            {
                handle = cached->second;
            }
            else
            {
                optional<MeshData> data = BuildShapeMeshData(primitive.Shape);
                if (!data)
                {
                    continue;
                }
                handle = manager.CreateAsync<Mesh>(Mesh::CreateAsync(
                    manager.GetContext(), manager.GetTasks(), std::move(*data), ShapeName(*key)));
                cache.Entries.emplace(*key, handle);
            }

            if (renderer == nullptr)
            {
                scene.Add<MeshRenderer>(entity);
            }
            scene.Get<MeshRenderer>(entity).Mesh = handle;
        }

        // Prune cache entries no MeshRenderer references, so a parameter dragged across many
        // values does not accumulate resident meshes — dropping the handle retires the mesh.
        std::unordered_set<Detail::AssetCacheEntry*> referenced;
        scene.Each<MeshRenderer>(
            [&](Entity, MeshRenderer& renderer)
            {
                if (const Ref<Detail::AssetCacheEntry> entry = AssetManager::EntryOf(renderer.Mesh))
                {
                    referenced.insert(entry.get());
                }
            });

        for (auto it = cache.Entries.begin(); it != cache.Entries.end();)
        {
            const Ref<Detail::AssetCacheEntry> entry = AssetManager::EntryOf(it->second);
            if (entry == nullptr || !referenced.contains(entry.get()))
            {
                it = cache.Entries.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}
