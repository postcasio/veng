#include "MutationTools.h"

#include "ReflectToJson.h"

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Resolve.h>
#include <Veng/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <charconv>

namespace Veng::Mcp
{
    using Json = nlohmann::json;

    namespace
    {
        /// @brief Reads an entity handle from `{ id: {index,generation} }` or a bare `{ index, generation }`.
        ///
        /// Returns Entity::Null when neither shape is present or the fields are not numbers, which
        /// the caller reports as a located error.
        Entity ParseEntity(const Json& fields)
        {
            if (!fields.is_object() || !fields.contains("index") || !fields["index"].is_number())
            {
                return Entity::Null;
            }
            Entity entity;
            entity.Index = fields["index"].get<u32>();
            entity.Generation = fields.contains("generation") && fields["generation"].is_number()
                                    ? fields["generation"].get<u32>()
                                    : 0;
            return entity;
        }

        /// @brief Reads the `id` argument (nested object or bare index/generation) into an Entity.
        Entity ParseTarget(const Json& args)
        {
            if (args.is_object() && args.contains("id") && args["id"].is_object())
            {
                return ParseEntity(args["id"]);
            }
            return ParseEntity(args);
        }

        /// @brief Builds the `{ index, generation }` object for an entity handle.
        Json EntityId(Entity entity)
        {
            return Json{{"index", entity.Index}, {"generation", entity.Generation}};
        }

        /// @brief Resolves a component `QualifiedName` to its TypeId, validated against the registry.
        ///
        /// A named type the registry does not hold is a located error, so an unknown agent-supplied
        /// name never reaches an asserting lookup.
        Result<TypeId> ResolveComponent(const string& name, const TypeRegistry& registry)
        {
            for (const auto& [id, info] : registry.All())
            {
                if (TypeNameMatches(info, name))
                {
                    return id;
                }
            }
            return std::unexpected(fmt::format("unknown component type '{}'", name));
        }

        /// @brief Resolves the world scene, or a located error when none is loaded.
        Result<Scene*> ResolveScene(const McpHost& host)
        {
            Scene* scene = host.CurrentWorld ? host.CurrentWorld() : nullptr;
            if (scene == nullptr)
            {
                return std::unexpected(string("no world is loaded"));
            }
            return scene;
        }

        /// @brief Rebuilds a MeshRenderer's Mesh from its inline recipe Source, matching the editor's ResolveEntity.
        ///
        /// A set_field or add_component that writes a non-empty MeshRenderer.Source needs the same
        /// recipe→mesh build the prefab populate pass and the editor's inspector edit perform, or
        /// the entity would render nothing until re-spawned.
        void RebuildMeshSourceIfPresent(Scene& scene, AssetManager& assets, Entity entity)
        {
            if (MeshRenderer* renderer = scene.TryGet<MeshRenderer>(entity))
            {
                if (renderer->Source.HasValue())
                {
                    renderer->Mesh = BuildPrimitiveMesh(assets, renderer->Source);
                }
            }
        }

        /// @brief Routes a mutation through McpHost::ApplyMutation, or returns false when the hook is null or declined.
        ///
        /// The tools consult this before applying an edit raw: a set hook that returns true handled
        /// the edit (an editor host pushed the corresponding CommandStack command); a null hook or a
        /// false return leaves the raw path to apply the edit directly to the scene.
        bool RouteMutation(const McpHost& host, const McpMutation& mutation)
        {
            return host.ApplyMutation && host.ApplyMutation(mutation);
        }
    }

    void RegisterMutationTools(McpServer& server, const McpHost& host)
    {
        // entity.add_component — default-construct a component onto an entity, then apply partial values.
        {
            McpTool tool;
            tool.Name = "entity.add_component";
            tool.Description =
                "Adds a component to an entity and applies optional field values. Argument: "
                "{ id: { index, generation }, component: <QualifiedName>, values?: { <field>: … } "
                "}. "
                "Errors if the component is already present or the type is unregistered.";
            tool.InputSchemaJson =
                R"({"type":"object","required":["id","component"],"properties":{"id":{"type":"object"},)"
                R"("component":{"type":"string"},"values":{"type":"object"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                const Result<Scene*> scene = ResolveScene(host);
                if (!scene)
                {
                    return std::unexpected(scene.error());
                }
                if (!args.is_object() || !args.contains("component") ||
                    !args["component"].is_string())
                {
                    return std::unexpected(string("expected { id, component: <QualifiedName> }"));
                }

                const Entity target = ParseTarget(args);
                if (target.IsNull() || !(*scene)->IsAlive(target))
                {
                    return std::unexpected(string("target entity is not live"));
                }

                const Result<TypeId> type =
                    ResolveComponent(args["component"].get<string>(), host.Types);
                if (!type)
                {
                    return std::unexpected(type.error());
                }
                if ((*scene)->TryGetComponent(target, *type) != nullptr)
                {
                    return std::unexpected(fmt::format("entity already has component '{}'",
                                                       host.Types.Info(*type).QualifiedName));
                }

                const Json values = args.contains("values") ? args["values"] : Json::object();

                McpMutation mutation;
                mutation.Kind = McpMutationKind::AddComponent;
                mutation.Target = target;
                mutation.Component = *type;
                mutation.Values = values.dump();
                if (!RouteMutation(host, mutation))
                {
                    void* component = (*scene)->AddComponent(target, *type);
                    const VoidResult applied =
                        JsonToFields(values, component, host.Types.Info(*type), host.Types);
                    if (!applied)
                    {
                        return std::unexpected(applied.error());
                    }
                    RebuildMeshSourceIfPresent(**scene, host.Assets, target);
                }
                return Json{{"id", EntityId(target)},
                            {"added", host.Types.Info(*type).QualifiedName}}
                    .dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // entity.remove_component — the type-erased remove; the Hierarchy link is not removable.
        {
            McpTool tool;
            tool.Name = "entity.remove_component";
            tool.Description = "Removes a component from an entity. Argument: "
                               "{ id: { index, generation }, component: <QualifiedName> }. The "
                               "Hierarchy component is not removable.";
            tool.InputSchemaJson =
                R"({"type":"object","required":["id","component"],"properties":{"id":{"type":"object"},)"
                R"("component":{"type":"string"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                const Result<Scene*> scene = ResolveScene(host);
                if (!scene)
                {
                    return std::unexpected(scene.error());
                }
                if (!args.is_object() || !args.contains("component") ||
                    !args["component"].is_string())
                {
                    return std::unexpected(string("expected { id, component: <QualifiedName> }"));
                }

                const Entity target = ParseTarget(args);
                if (target.IsNull() || !(*scene)->IsAlive(target))
                {
                    return std::unexpected(string("target entity is not live"));
                }

                const Result<TypeId> type =
                    ResolveComponent(args["component"].get<string>(), host.Types);
                if (!type)
                {
                    return std::unexpected(type.error());
                }
                if (*type == host.Types.IdOf<Hierarchy>())
                {
                    return std::unexpected(string("the Hierarchy component is not removable"));
                }
                if ((*scene)->TryGetComponent(target, *type) == nullptr)
                {
                    return std::unexpected(fmt::format("entity does not have component '{}'",
                                                       host.Types.Info(*type).QualifiedName));
                }

                McpMutation mutation;
                mutation.Kind = McpMutationKind::RemoveComponent;
                mutation.Target = target;
                mutation.Component = *type;
                if (!RouteMutation(host, mutation))
                {
                    (*scene)->RemoveComponent(target, *type);
                }
                return Json{{"id", EntityId(target)},
                            {"removed", host.Types.Info(*type).QualifiedName}}
                    .dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // entity.set_field — a partial field update over an existing component (bumps the spatial version).
        {
            McpTool tool;
            tool.Name = "entity.set_field";
            tool.Description =
                "Applies a partial field update to a component on an entity. Argument: "
                "{ id: { index, generation }, component: <QualifiedName>, values: { <field>: … } "
                "}. "
                "Errors if the component is absent.";
            tool.InputSchemaJson =
                R"({"type":"object","required":["id","component","values"],"properties":)"
                R"({"id":{"type":"object"},"component":{"type":"string"},"values":{"type":"object"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                const Result<Scene*> scene = ResolveScene(host);
                if (!scene)
                {
                    return std::unexpected(scene.error());
                }
                if (!args.is_object() || !args.contains("component") ||
                    !args["component"].is_string() || !args.contains("values") ||
                    !args["values"].is_object())
                {
                    return std::unexpected(
                        string("expected { id, component: <QualifiedName>, values: {…} }"));
                }

                const Entity target = ParseTarget(args);
                if (target.IsNull() || !(*scene)->IsAlive(target))
                {
                    return std::unexpected(string("target entity is not live"));
                }

                const Result<TypeId> type =
                    ResolveComponent(args["component"].get<string>(), host.Types);
                if (!type)
                {
                    return std::unexpected(type.error());
                }
                if ((*scene)->TryGetComponent(target, *type) == nullptr)
                {
                    return std::unexpected(fmt::format("entity does not have component '{}'",
                                                       host.Types.Info(*type).QualifiedName));
                }

                const Json& values = args["values"];

                McpMutation mutation;
                mutation.Kind = McpMutationKind::SetField;
                mutation.Target = target;
                mutation.Component = *type;
                mutation.Values = values.dump();
                if (!RouteMutation(host, mutation))
                {
                    // The non-const TryGetComponent access bumps the spatial version, so the
                    // broadphase rebuilds after a geometry edit with no extra bookkeeping.
                    void* component = (*scene)->TryGetComponent(target, *type);
                    const VoidResult applied =
                        JsonToFields(values, component, host.Types.Info(*type), host.Types);
                    if (!applied)
                    {
                        return std::unexpected(applied.error());
                    }
                    RebuildMeshSourceIfPresent(**scene, host.Assets, target);
                }
                return Json{{"id", EntityId(target)},
                            {"updated", host.Types.Info(*type).QualifiedName}}
                    .dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // entity.spawn — create an entity, add its named components, and optionally parent it.
        {
            McpTool tool;
            tool.Name = "entity.spawn";
            tool.Description =
                "Creates an entity and returns its id. Argument: "
                "{ name?, parent?: { index, generation }, components?: { <QualifiedName>: {…}, … } "
                "}. "
                "Adds a Name when 'name' is given and each named component seeded from its values.";
            tool.InputSchemaJson = R"({"type":"object","properties":{"name":{"type":"string"},)"
                                   R"("parent":{"type":"object"},"components":{"type":"object"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                const Result<Scene*> scene = ResolveScene(host);
                if (!scene)
                {
                    return std::unexpected(scene.error());
                }

                Entity parent = Entity::Null;
                if (args.is_object() && args.contains("parent") && args["parent"].is_object())
                {
                    parent = ParseEntity(args["parent"]);
                    if (parent.IsNull() || !(*scene)->IsAlive(parent))
                    {
                        return std::unexpected(string("parent entity is not live"));
                    }
                }

                const string name =
                    args.is_object() && args.contains("name") && args["name"].is_string()
                        ? args["name"].get<string>()
                        : string{};

                const Json components = args.is_object() && args.contains("components") &&
                                                args["components"].is_object()
                                            ? args["components"]
                                            : Json::object();

                // Resolve every named component type up front so an unknown name errors before the
                // entity is created — no half-built entity left behind on a bad request.
                for (const auto& [key, value] : components.items())
                {
                    const Result<TypeId> type = ResolveComponent(key, host.Types);
                    if (!type)
                    {
                        return std::unexpected(type.error());
                    }
                    if (!value.is_object())
                    {
                        return std::unexpected(
                            fmt::format("component '{}' values must be an object", key));
                    }
                }

                McpMutation mutation;
                mutation.Kind = McpMutationKind::SpawnEntity;
                mutation.Target = parent;
                mutation.Name = name;
                mutation.Components = components.dump();
                if (RouteMutation(host, mutation))
                {
                    // The editor host owns the spawned handle and reports it back through the hook
                    // is out of scope here — a routed spawn returns success without a resolved id.
                    return Json{{"spawned", true}}.dump();
                }

                const Entity entity = (*scene)->CreateEntity();
                if (!name.empty())
                {
                    (*scene)->Add<Name>(entity, Name{.Value = name});
                }
                for (const auto& [key, value] : components.items())
                {
                    const Result<TypeId> type = ResolveComponent(key, host.Types);
                    void* component = (*scene)->AddComponent(entity, *type);
                    const VoidResult applied =
                        JsonToFields(value, component, host.Types.Info(*type), host.Types);
                    if (!applied)
                    {
                        return std::unexpected(applied.error());
                    }
                }
                RebuildMeshSourceIfPresent(**scene, host.Assets, entity);
                if (!parent.IsNull())
                {
                    (*scene)->SetParent(entity, parent);
                }
                return Json{{"id", EntityId(entity)}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // entity.destroy — recursive destroy; returns the count of entities removed.
        {
            McpTool tool;
            tool.Name = "entity.destroy";
            tool.Description = "Destroys an entity and its Hierarchy subtree. Argument: "
                               "{ id: { index, generation } }. Returns the count destroyed.";
            tool.InputSchemaJson =
                R"({"type":"object","required":["id"],"properties":{"id":{"type":"object"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                const Result<Scene*> scene = ResolveScene(host);
                if (!scene)
                {
                    return std::unexpected(scene.error());
                }

                const Entity target = ParseTarget(args);
                if (target.IsNull() || !(*scene)->IsAlive(target))
                {
                    return std::unexpected(string("target entity is not live"));
                }

                // Count the subtree before destroying it — DestroyEntity is recursive over the
                // Hierarchy links, so walk them the same way for the reported count.
                usize destroyed = 0;
                const function<void(Entity)> countSubtree = [&](Entity entity)
                {
                    ++destroyed;
                    (*scene)->ForEachChild(entity, [&](Entity child) { countSubtree(child); });
                };
                countSubtree(target);

                McpMutation mutation;
                mutation.Kind = McpMutationKind::DestroyEntity;
                mutation.Target = target;
                if (!RouteMutation(host, mutation))
                {
                    (*scene)->DestroyEntity(target);
                }
                return Json{{"destroyed", destroyed}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // world.load_prefab — load a resident prefab and spawn it, optionally under a parent.
        {
            McpTool tool;
            tool.Name = "world.load_prefab";
            tool.Description =
                "Loads a prefab asset and spawns it into the world. Argument: "
                "{ asset: <AssetId>, parent?: { index, generation } }. Returns the spawned root "
                "ids. A missing asset is an error (the runtime cooks nothing on demand).";
            tool.InputSchemaJson = R"({"type":"object","required":["asset"],"properties":)"
                                   R"({"asset":{"type":"string"},"parent":{"type":"object"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                const Result<Scene*> scene = ResolveScene(host);
                if (!scene)
                {
                    return std::unexpected(scene.error());
                }
                if (!args.is_object() || !args.contains("asset"))
                {
                    return std::unexpected(string("expected { asset: <AssetId> }"));
                }

                u64 rawId = 0;
                const Json& asset = args["asset"];
                if (asset.is_string())
                {
                    const string text = asset.get<string>();
                    if (std::from_chars(text.data(), text.data() + text.size(), rawId).ec !=
                        std::errc{})
                    {
                        return std::unexpected(fmt::format("invalid AssetId '{}'", text));
                    }
                }
                else if (asset.is_number_unsigned())
                {
                    rawId = asset.get<u64>();
                }
                else
                {
                    return std::unexpected(
                        string("'asset' must be an AssetId as a decimal string or number"));
                }
                const AssetId assetId{rawId};

                Entity parent = Entity::Null;
                if (args.contains("parent") && args["parent"].is_object())
                {
                    parent = ParseEntity(args["parent"]);
                    if (parent.IsNull() || !(*scene)->IsAlive(parent))
                    {
                        return std::unexpected(string("parent entity is not live"));
                    }
                }

                McpMutation mutation;
                mutation.Kind = McpMutationKind::LoadPrefab;
                mutation.Target = parent;
                mutation.Asset = assetId;
                if (RouteMutation(host, mutation))
                {
                    return Json{{"spawned", true}}.dump();
                }

                const AssetResult<AssetHandle<Prefab>> prefab =
                    host.Assets.LoadSync<Prefab>(assetId);
                if (!prefab)
                {
                    return std::unexpected(fmt::format("prefab {} did not load", rawId));
                }

                const Prefab::SpawnResult spawned = (*prefab)->SpawnInto(**scene, host.Assets);
                Json roots = Json::array();
                for (const Entity root : spawned.Roots)
                {
                    if (!parent.IsNull())
                    {
                        (*scene)->SetParent(root, parent);
                    }
                    roots.push_back(EntityId(root));
                }
                return Json{{"roots", std::move(roots)}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }
    }
}
