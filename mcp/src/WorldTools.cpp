#include "WorldTools.h"

#include "ReflectToJson.h"

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>

namespace Veng::Mcp
{
    using Json = nlohmann::json;

    namespace
    {
        /// @brief Default page size for the list tools when the caller omits `limit`.
        ///
        /// A cap on how much of an unbounded world one call dumps into an agent's context
        /// (a volume convention for the single trusted local client, not a DoS defense); the
        /// caller pages the tail through nextCursor.
        constexpr u32 DefaultLimit = 200;

        /// @brief Parses the optional `limit` argument, clamped to a sensible page cap.
        u32 ParseLimit(const Json& args)
        {
            if (!args.is_object() || !args.contains("limit") || !args["limit"].is_number())
            {
                return DefaultLimit;
            }
            const i64 requested = args["limit"].get<i64>();
            if (requested <= 0)
            {
                return DefaultLimit;
            }
            return static_cast<u32>(std::min<i64>(requested, DefaultLimit));
        }

        /// @brief Parses the opaque `cursor` (the resume entity slot index), defaulting to 0.
        u32 ParseCursor(const Json& args)
        {
            if (!args.is_object() || !args.contains("cursor"))
            {
                return 0;
            }
            const Json& cursor = args["cursor"];
            if (cursor.is_number())
            {
                const i64 value = cursor.get<i64>();
                return value < 0 ? 0 : static_cast<u32>(value);
            }
            // The cursor is emitted as a string so a large slot index round-trips exactly.
            if (cursor.is_string())
            {
                const string text = cursor.get<string>();
                u64 value = 0;
                if (std::from_chars(text.data(), text.data() + text.size(), value).ec ==
                    std::errc{})
                {
                    return static_cast<u32>(value);
                }
            }
            return 0;
        }

        /// @brief Reads an entity handle from the tool argument (`{ id: {index,generation} }` or `{ index, generation }`).
        ///
        /// Returns Entity::Null when neither shape is present or the fields are not numbers,
        /// which the caller reports as an isError result.
        Entity ParseEntity(const Json& args)
        {
            const Json* fields = &args;
            if (args.is_object() && args.contains("id") && args["id"].is_object())
            {
                fields = &args["id"];
            }
            if (!fields->is_object() || !fields->contains("index") ||
                !(*fields)["index"].is_number())
            {
                return Entity::Null;
            }
            Entity entity;
            entity.Index = (*fields)["index"].get<u32>();
            entity.Generation =
                fields->contains("generation") && (*fields)["generation"].is_number()
                    ? (*fields)["generation"].get<u32>()
                    : 0;
            return entity;
        }

        /// @brief Builds the `{ index, generation }` object for an entity handle.
        Json EntityId(Entity entity)
        {
            return Json{{"index", entity.Index}, {"generation", entity.Generation}};
        }

        /// @brief Returns the entity's Name component value, or an empty string when it has none.
        string EntityName(const Scene& scene, Entity entity)
        {
            if (const Name* name = scene.TryGet<Name>(entity))
            {
                return name->Value;
            }
            return {};
        }

        /// @brief Collects an entity's component QualifiedNames via ForEachComponent.
        ///
        /// Uses the non-const ForEachComponent (the only enumeration form); it bumps the
        /// spatial version if the entity holds a spatial component, which a read-only tool
        /// accepts as harmless here (no downstream broadphase runs at the pump point).
        Json ComponentNames(Scene& scene, const TypeRegistry& registry, Entity entity)
        {
            Json out = Json::array();
            scene.ForEachComponent(entity,
                                   [&](TypeId type, void*)
                                   {
                                       if (registry.IsRegistered(type))
                                       {
                                           out.push_back(registry.Info(type).QualifiedName);
                                       }
                                   });
            return out;
        }

        /// @brief Resolves a component `QualifiedName` argument to its TypeId, validated against the registry.
        ///
        /// Returns InvalidTypeId when the argument is absent, or an error string when it names
        /// a type the registry does not hold — so an unknown agent-supplied name never reaches
        /// an asserting lookup, and a missing filter is "no filter".
        Result<TypeId> ResolveComponentFilter(const Json& args, const TypeRegistry& registry)
        {
            if (!args.is_object() || !args.contains("component"))
            {
                return InvalidTypeId;
            }
            if (!args["component"].is_string())
            {
                return std::unexpected(string("'component' must be a type name string"));
            }
            const string key = args["component"].get<string>();
            for (const auto& [id, info] : registry.All())
            {
                if (TypeNameMatches(info, key))
                {
                    return id;
                }
            }
            return std::unexpected(fmt::format("unknown component type '{}'", key));
        }

        /// @brief Pages the scene's entities into a JSON list, filtered by an optional component TypeId.
        ///
        /// Visits entities in slot-index order from the cursor, includes an entity when it
        /// passes the filter (InvalidTypeId = no filter), and stops after `limit`. Sets
        /// nextCursor (the next slot index) while more entities remain past the page.
        /// @param full  When true, each item carries its component list; when false, just id + name.
        Json PageEntities(Scene& scene, const TypeRegistry& registry, u32 cursor, u32 limit,
                          TypeId filter, bool full)
        {
            Json items = Json::array();
            u32 nextCursor = 0;
            bool more = false;

            scene.ForEachEntity(
                [&](Entity entity)
                {
                    if (entity.Index < cursor)
                    {
                        return;
                    }
                    if (filter != InvalidTypeId && scene.TryGetComponent(entity, filter) == nullptr)
                    {
                        return;
                    }
                    if (items.size() >= limit)
                    {
                        // One entity past the page: remember where to resume and stop including.
                        if (!more)
                        {
                            more = true;
                            nextCursor = entity.Index;
                        }
                        return;
                    }

                    Json item{{"id", EntityId(entity)}, {"name", EntityName(scene, entity)}};
                    if (full)
                    {
                        item["components"] = ComponentNames(scene, registry, entity);
                    }
                    items.push_back(std::move(item));
                });

            Json out{{"entities", std::move(items)}};
            if (more)
            {
                out["nextCursor"] = std::to_string(nextCursor);
            }
            return out;
        }
    }

    void RegisterWorldTools(McpServer& server, const McpHost& host)
    {
        // world.list_entities — the full listing (ids, names, component type names), paginated,
        // with an optional `component` filter.
        {
            McpTool tool;
            tool.Name = "world.list_entities";
            tool.Description = "Lists the current world's entities (id, name, component types), "
                               "paginated. Optional 'component' filters to entities having a "
                               "component type (its QualifiedName).";
            tool.InputSchemaJson =
                R"({"type":"object","properties":{"component":{"type":"string"},)"
                R"("limit":{"type":"integer"},"cursor":{"type":"string"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                Scene* scene = host.CurrentWorld ? host.CurrentWorld() : nullptr;
                if (scene == nullptr)
                {
                    return Json{{"entities", Json::array()}}.dump();
                }

                const Result<TypeId> filter = ResolveComponentFilter(args, host.Types);
                if (!filter)
                {
                    return std::unexpected(filter.error());
                }
                return PageEntities(*scene, host.Types, ParseCursor(args), ParseLimit(args),
                                    *filter,
                                    /*full=*/true)
                    .dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // entity.get — the full component dump for one entity, validated against the scene.
        {
            McpTool tool;
            tool.Name = "entity.get";
            tool.Description = "Dumps one entity's components as JSON, keyed by component "
                               "QualifiedName. Argument: { id: { index, generation } } (or bare "
                               "{ index, generation }).";
            tool.InputSchemaJson =
                R"({"type":"object","properties":{"id":{"type":"object"},)"
                R"("index":{"type":"integer"},"generation":{"type":"integer"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                Scene* scene = host.CurrentWorld ? host.CurrentWorld() : nullptr;
                if (scene == nullptr)
                {
                    return std::unexpected(string("no world is loaded"));
                }

                const Entity entity = ParseEntity(args);
                if (entity.IsNull())
                {
                    return std::unexpected(string("expected { id: { index, generation } }"));
                }
                if (!scene->IsAlive(entity))
                {
                    return std::unexpected(
                        fmt::format("entity {{ index: {}, generation: {} }} is not live",
                                    entity.Index, entity.Generation));
                }

                Json components = Json::object();
                scene->ForEachComponent(entity,
                                        [&](TypeId type, void* component)
                                        {
                                            if (!host.Types.IsRegistered(type))
                                            {
                                                return;
                                            }
                                            const TypeInfo& info = host.Types.Info(type);
                                            components[info.QualifiedName] =
                                                FieldsToJson(component, info, host.Types);
                                        });

                return Json{{"id", EntityId(entity)},
                            {"name", EntityName(*scene, entity)},
                            {"components", std::move(components)}}
                    .dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // world.query — the entities having a given component type (ids + names), paginated.
        {
            McpTool tool;
            tool.Name = "world.query";
            tool.Description = "Lists entities (id, name) having a component type. Argument: "
                               "{ component: <QualifiedName>, limit?, cursor? }.";
            tool.InputSchemaJson = R"({"type":"object","required":["component"],"properties":)"
                                   R"({"component":{"type":"string"},"limit":{"type":"integer"},)"
                                   R"("cursor":{"type":"string"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                if (!args.is_object() || !args.contains("component"))
                {
                    return std::unexpected(string("'component' is required"));
                }

                const Result<TypeId> filter = ResolveComponentFilter(args, host.Types);
                if (!filter)
                {
                    return std::unexpected(filter.error());
                }

                Scene* scene = host.CurrentWorld ? host.CurrentWorld() : nullptr;
                if (scene == nullptr)
                {
                    return Json{{"entities", Json::array()}}.dump();
                }
                return PageEntities(*scene, host.Types, ParseCursor(args), ParseLimit(args),
                                    *filter,
                                    /*full=*/false)
                    .dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // scene.stats — cheap situational awareness (entity count, spatial version, bounds).
        {
            McpTool tool;
            tool.Name = "scene.stats";
            tool.Description = "Reports the current world's entity count, spatial version, and "
                               "world-space bounds.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view) -> Result<string>
            {
                const Scene* scene = host.CurrentWorld ? host.CurrentWorld() : nullptr;
                if (scene == nullptr)
                {
                    return Json{{"entity_count", 0}}.dump();
                }

                const AABB bounds = SceneBounds(*scene);
                return Json{{"entity_count", scene->EntityCount()},
                            {"spatial_version", scene->GetSpatialVersion()},
                            {"bounds",
                             {{"min", Json::array({bounds.Min.x, bounds.Min.y, bounds.Min.z})},
                              {"max", Json::array({bounds.Max.x, bounds.Max.y, bounds.Max.z})}}}}
                    .dump();
            };
            server.RegisterTool(std::move(tool));
        }
    }
}
