#include <Veng/Scene/Sockets.h>

#include <algorithm>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include "../Asset/Loaders/PrefabLoader.h"

namespace Veng
{
    const MeshSocket* FindMeshSocket(const Scene& scene, const Entity meshEntity,
                                     const std::string_view socketName)
    {
        if (meshEntity.IsNull() || !scene.IsAlive(meshEntity))
        {
            return nullptr;
        }

        const auto* renderer = scene.TryGet<MeshRenderer>(meshEntity);
        if (renderer == nullptr || !renderer->Mesh.IsLoaded())
        {
            return nullptr;
        }

        return renderer->Mesh.Get()->FindSocket(socketName);
    }

    bool AttachToSocket(Scene& scene, const Entity child, const Entity meshEntity,
                        const std::string_view socketName)
    {
        const MeshSocket* socket = FindMeshSocket(scene, meshEntity, socketName);
        if (socket == nullptr)
        {
            return false;
        }

        scene.SetParent(child, meshEntity);

        Transform& transform =
            scene.Has<Transform>(child) ? scene.Get<Transform>(child) : scene.Add<Transform>(child);
        transform.Position = socket->Position;
        transform.Rotation = socket->Rotation;
        transform.Scale = socket->Scale;
        return true;
    }

    namespace
    {
        constexpr u32 NoParent = ~0u;

        // One spawned entity as a prefab walk sees it: only the parts that place a mesh or a socket.
        struct FlatEntity
        {
            string Name;
            Transform Local;
            // Index into the flattened list; NoParent for a root.
            u32 Parent = NoParent;
            // The cooked mesh the entity draws; invalid when it draws none.
            AssetId Mesh;
        };

        AssetLoadError Corrupt(const AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }

        // Decodes a component record into a fresh T — a whole-component replace, matching how
        // SpawnInto applies a nesting entity's overrides.
        template <typename T>
        Result<T> ReadRecord(const Prefab::Component& component, const TypeRegistry& types)
        {
            if (!types.IsRegistered(component.Type))
            {
                return std::unexpected(fmt::format(
                    "component type {:#018x} is not registered with the manager's TypeRegistry",
                    component.Type));
            }

            T value{};
            const VoidResult read =
                ReadFields(component.Record, &value, types.Info(component.Type), types);
            if (!read)
            {
                return std::unexpected(read.error());
            }
            return value;
        }

        // Applies one entity's records to its flattened slot. `slots` maps this prefab's authored
        // indices to flattened ones, which is what a Hierarchy parent reference is resolved against.
        VoidResult ApplyComponents(const Prefab::PrefabEntity& entity, FlatEntity& flat,
                                   std::span<const u32> slots, const TypeRegistry& types)
        {
            for (const Prefab::Component& component : entity.Components)
            {
                if (component.Type == TypeIdOf<Name>())
                {
                    Result<Name> name = ReadRecord<Name>(component, types);
                    if (!name)
                    {
                        return std::unexpected(name.error());
                    }
                    flat.Name = std::move(name->Value);
                }
                else if (component.Type == TypeIdOf<Transform>())
                {
                    const Result<Transform> transform = ReadRecord<Transform>(component, types);
                    if (!transform)
                    {
                        return std::unexpected(transform.error());
                    }
                    flat.Local = *transform;
                }
                else if (component.Type == TypeIdOf<Hierarchy>())
                {
                    const Result<Hierarchy> link = ReadRecord<Hierarchy>(component, types);
                    if (!link)
                    {
                        return std::unexpected(link.error());
                    }
                    const u32 parent = link->Parent.Index;
                    if (parent == Entity::InvalidIndex)
                    {
                        flat.Parent = NoParent;
                    }
                    else if (parent < slots.size())
                    {
                        flat.Parent = slots[parent];
                    }
                    else
                    {
                        return std::unexpected(fmt::format(
                            "parent index {} out of range ({} entities)", parent, slots.size()));
                    }
                }
                else if (component.Type == TypeIdOf<MeshRenderer>())
                {
                    const Result<MeshRenderer> renderer =
                        ReadRecord<MeshRenderer>(component, types);
                    if (!renderer)
                    {
                        return std::unexpected(renderer.error());
                    }
                    const bool recipe = renderer->Source.ActiveType() != InvalidTypeId;
                    flat.Mesh = recipe ? AssetId{} : renderer->Mesh.Id();
                }
            }
            return {};
        }

        // Appends `prefab`'s entities to `nodes`, expanding nested prefabs as SpawnInto does, and
        // returns the flattened indices of its roots in authored order. `chain` holds the prefabs
        // being expanded, so a nesting cycle past the cook is an error rather than a recursion.
        AssetResult<vector<u32>> Flatten(const AssetManager& assets, const AssetId prefab,
                                         vector<AssetId>& chain, vector<FlatEntity>& nodes)
        {
            if (std::ranges::find(chain, prefab) != chain.end())
            {
                return std::unexpected(
                    Corrupt(prefab, fmt::format("prefab {} nests itself", prefab.Value)));
            }

            const AssetResult<std::span<const u8>> cooked =
                assets.ReadCooked(AssetTypes::Prefab, prefab);
            if (!cooked)
            {
                return std::unexpected(cooked.error());
            }

            Result<vector<Prefab::PrefabEntity>> decoded = DecodeCookedPrefab(*cooked);
            if (!decoded)
            {
                return std::unexpected(Corrupt(prefab, std::move(decoded.error())));
            }
            const vector<Prefab::PrefabEntity>& entities = *decoded;

            chain.push_back(prefab);

            // Every entity gets its slot before any record is read, since a Hierarchy parent may
            // name a later entity. A nesting entity's slot is its body's first root.
            vector<u32> slots(entities.size());
            vector<vector<u32>> bodyRoots(entities.size());
            for (usize i = 0; i < entities.size(); ++i)
            {
                if (entities[i].NestedPrefab.IsValid())
                {
                    AssetResult<vector<u32>> body =
                        Flatten(assets, entities[i].NestedPrefab, chain, nodes);
                    if (!body)
                    {
                        return std::unexpected(body.error());
                    }
                    if (!body->empty())
                    {
                        slots[i] = body->front();
                        bodyRoots[i].assign(body->begin() + 1, body->end());
                        continue;
                    }
                }
                slots[i] = static_cast<u32>(nodes.size());
                nodes.emplace_back();
            }

            chain.pop_back();

            const TypeRegistry& types = assets.GetTypeRegistry();
            for (usize i = 0; i < entities.size(); ++i)
            {
                const VoidResult applied =
                    ApplyComponents(entities[i], nodes[slots[i]], slots, types);
                if (!applied)
                {
                    return std::unexpected(Corrupt(prefab, applied.error()));
                }
            }

            for (usize i = 0; i < entities.size(); ++i)
            {
                for (const u32 root : bodyRoots[i])
                {
                    nodes[root].Parent = slots[i];
                }
            }

            vector<u32> roots;
            for (const u32 slot : slots)
            {
                if (nodes[slot].Parent == NoParent)
                {
                    roots.push_back(slot);
                }
            }
            return roots;
        }

        // A prefab flattened as a spawn lays it out.
        struct FlatPrefab
        {
            vector<FlatEntity> Nodes;
            // Each entity's frame relative to its root, the root's own Transform excluded; filled
            // lazily by RootFrame.
            vector<optional<mat4>> Frames;
        };

        AssetResult<FlatPrefab> ReadFlatPrefab(const AssetManager& assets, const AssetId prefab)
        {
            FlatPrefab flat;
            vector<AssetId> chain;
            const AssetResult<vector<u32>> roots = Flatten(assets, prefab, chain, flat.Nodes);
            if (!roots)
            {
                return std::unexpected(roots.error());
            }
            flat.Frames.resize(flat.Nodes.size());
            return flat;
        }

        // Composes `start`'s frame up the chain, caching every frame it passes; a walk longer than
        // the entity count is a parent cycle.
        Result<mat4> RootFrame(FlatPrefab& flat, const u32 start)
        {
            const vector<FlatEntity>& nodes = flat.Nodes;
            vector<optional<mat4>>& frames = flat.Frames;
            vector<u32> chainUp;
            u32 current = start;
            while (!frames[current] && nodes[current].Parent != NoParent)
            {
                if (chainUp.size() > nodes.size())
                {
                    return std::unexpected(string("entity hierarchy forms a cycle"));
                }
                chainUp.push_back(current);
                current = nodes[current].Parent;
            }
            if (!frames[current])
            {
                frames[current] = mat4(1.0f);
            }
            for (usize i = chainUp.size(); i-- > 0;)
            {
                const u32 entity = chainUp[i];
                frames[entity] = *frames[nodes[entity].Parent] * LocalMatrix(nodes[entity].Local);
            }
            return *frames[start];
        }

        // Splits an affine matrix with no shear back into a Transform.
        Transform ToTransform(const mat4& matrix)
        {
            const vec3 x(matrix[0]);
            const vec3 y(matrix[1]);
            const vec3 z(matrix[2]);
            vec3 scale(glm::length(x), glm::length(y), glm::length(z));
            // A mirrored frame keeps a proper rotation by carrying the reflection in one scale axis.
            if (glm::dot(glm::cross(x, y), z) < 0.0f)
            {
                scale.x = -scale.x;
            }

            const auto axis = [](const vec3& column, const f32 length, const vec3& fallback)
            { return length != 0.0f ? column / length : fallback; };
            const mat3 rotation(axis(x, scale.x, vec3(1.0f, 0.0f, 0.0f)),
                                axis(y, scale.y, vec3(0.0f, 1.0f, 0.0f)),
                                axis(z, scale.z, vec3(0.0f, 0.0f, 1.0f)));

            return Transform{
                .Position = vec3(matrix[3]),
                .Rotation = glm::normalize(glm::quat_cast(rotation)),
                .Scale = scale,
            };
        }
    }

    AssetResult<vector<PrefabSocket>> ReadPrefabSockets(const AssetManager& assets,
                                                        const AssetId prefab)
    {
        AssetResult<FlatPrefab> flat = ReadFlatPrefab(assets, prefab);
        if (!flat)
        {
            return std::unexpected(flat.error());
        }
        const vector<FlatEntity>& nodes = flat->Nodes;

        map<u64, vector<MeshSocket>> meshSockets;
        vector<PrefabSocket> sockets;
        for (u32 i = 0; i < nodes.size(); ++i)
        {
            const FlatEntity& node = nodes[i];
            if (!node.Mesh.IsValid())
            {
                continue;
            }

            auto found = meshSockets.find(node.Mesh.Value);
            if (found == meshSockets.end())
            {
                AssetResult<vector<MeshSocket>> read = assets.ReadMeshSockets(node.Mesh);
                if (!read)
                {
                    return std::unexpected(read.error());
                }
                found = meshSockets.emplace(node.Mesh.Value, std::move(*read)).first;
            }
            if (found->second.empty())
            {
                continue;
            }

            const Result<mat4> frame = RootFrame(*flat, i);
            if (!frame)
            {
                return std::unexpected(Corrupt(prefab, frame.error()));
            }

            for (const MeshSocket& socket : found->second)
            {
                const Transform local{.Position = socket.Position,
                                      .Rotation = socket.Rotation,
                                      .Scale = socket.Scale};
                sockets.push_back(PrefabSocket{
                    .EntityName = node.Name,
                    .Mesh = node.Mesh,
                    .Local = socket,
                    .RootSpace = ToTransform(*frame * LocalMatrix(local)),
                });
            }
        }

        std::ranges::stable_sort(sockets,
                                 [](const PrefabSocket& a, const PrefabSocket& b)
                                 {
                                     if (a.EntityName != b.EntityName)
                                     {
                                         return a.EntityName < b.EntityName;
                                     }
                                     return a.Local.Name < b.Local.Name;
                                 });
        return sockets;
    }

    AssetResult<vector<PrefabMesh>> ReadPrefabMeshes(const AssetManager& assets,
                                                     const AssetId prefab)
    {
        AssetResult<FlatPrefab> flat = ReadFlatPrefab(assets, prefab);
        if (!flat)
        {
            return std::unexpected(flat.error());
        }

        vector<PrefabMesh> meshes;
        for (u32 i = 0; i < flat->Nodes.size(); ++i)
        {
            const FlatEntity& node = flat->Nodes[i];
            if (!node.Mesh.IsValid())
            {
                continue;
            }

            const Result<mat4> frame = RootFrame(*flat, i);
            if (!frame)
            {
                return std::unexpected(Corrupt(prefab, frame.error()));
            }
            meshes.push_back(PrefabMesh{
                .EntityName = node.Name,
                .Mesh = node.Mesh,
                .RootSpace = ToTransform(*frame),
            });
        }

        std::ranges::stable_sort(meshes, {}, &PrefabMesh::EntityName);
        return meshes;
    }
}
