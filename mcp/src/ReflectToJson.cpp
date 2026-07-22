#include "ReflectToJson.h"

#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Entity.h>

#include <nlohmann/json.hpp>

namespace Veng::Mcp
{
    using Json = nlohmann::json;

    namespace
    {
        /// @brief MCP's entity-addressing hooks: an Entity reads/writes as `{ index, generation }`.
        ///
        /// The only policy the shared walker needs from MCP — AssetId validation stays the
        /// default (accept every id; residency is the runtime's job).
        JsonFieldHooks MakeHooks()
        {
            JsonFieldHooks hooks;
            hooks.ReadReference = [](const Json& value) -> Result<Entity>
            {
                if (!value.is_object() || !value.contains("index") || !value["index"].is_number())
                {
                    return std::unexpected(
                        string("entity reference expects { index, generation }"));
                }
                Entity entity;
                entity.Index = value["index"].get<u32>();
                entity.Generation = value.contains("generation") && value["generation"].is_number()
                                        ? value["generation"].get<u32>()
                                        : 0;
                return entity;
            };
            hooks.WriteReference = [](Entity entity) -> Json
            { return Json{{"index", entity.Index}, {"generation", entity.Generation}}; };
            return hooks;
        }
    }

    Json FieldsToJson(const void* obj, const TypeInfo& type, const TypeRegistry& registry)
    {
        return JsonWriteFields(obj, type, registry, MakeHooks());
    }

    VoidResult JsonToFields(const Json& source, void* obj, const TypeInfo& type,
                            const TypeRegistry& registry)
    {
        return JsonReadFields(obj, type, source, registry, MakeHooks(),
                              /*allowUnknownFields=*/true);
    }
}
