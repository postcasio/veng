#include "ReflectToJson.h"

#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Entity.h>

#include <charconv>

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

        /// @brief Rewrites every AssetHandle leaf in `value` between the walker's raw JSON
        /// integer and MCP's decimal-string wire encoding.
        ///
        /// A 64-bit AssetId loses precision as a bare JSON number past 2^53; MCP's public
        /// contract is a decimal string, so this walks the JSON tree in lockstep with the
        /// type's FieldDescriptors (the shared walker already shaped it identically) and
        /// re-encodes each AssetHandle value in place. `toWire` selects the direction: true
        /// converts a written document's numbers to strings, false parses a source
        /// document's strings back to numbers before JsonReadFields sees them.
        void ConvertAssetHandles(Json& value, const TypeInfo& type, const TypeRegistry& registry,
                                 bool toWire);

        void ConvertAssetHandleValue(Json& value, bool toWire)
        {
            if (toWire)
            {
                if (value.is_number_unsigned())
                {
                    value = std::to_string(value.get<u64>());
                }
                return;
            }
            if (value.is_string())
            {
                const string text = value.get<string>();
                u64 id = 0;
                if (std::from_chars(text.data(), text.data() + text.size(), id).ec == std::errc{})
                {
                    value = id;
                }
            }
        }

        void ConvertAssetHandles(Json& value, const TypeInfo& type, const TypeRegistry& registry,
                                 bool toWire)
        {
            if (!value.is_object())
            {
                return;
            }
            for (const FieldDescriptor& field : type.Fields)
            {
                const auto it = value.find(field.Name);
                if (it == value.end())
                {
                    continue;
                }
                switch (field.Class)
                {
                case FieldClass::AssetHandle:
                    ConvertAssetHandleValue(*it, toWire);
                    break;
                case FieldClass::Struct:
                    if (registry.IsRegistered(field.Type))
                    {
                        ConvertAssetHandles(*it, registry.Info(field.Type), registry, toWire);
                    }
                    break;
                case FieldClass::Variant:
                    if (it->is_object() && it->contains("value") && (*it)["value"].is_object())
                    {
                        const TypeInfo& info = registry.Info(field.Type);
                        const string typeName = it->value("type", string{});
                        for (const TypeId altId : info.VariantAlternatives)
                        {
                            if (registry.IsRegistered(altId) &&
                                TypeNameMatches(registry.Info(altId), typeName))
                            {
                                ConvertAssetHandles((*it)["value"], registry.Info(altId), registry,
                                                    toWire);
                                break;
                            }
                        }
                    }
                    break;
                case FieldClass::Array:
                    if (it->is_array() && registry.IsRegistered(field.ElementType))
                    {
                        const TypeInfo& element = registry.Info(field.ElementType);
                        for (Json& item : *it)
                        {
                            if (field.ElementType != InvalidTypeId &&
                                element.Class == FieldClass::AssetHandle)
                            {
                                ConvertAssetHandleValue(item, toWire);
                            }
                            else if (element.Class == FieldClass::Struct)
                            {
                                ConvertAssetHandles(item, element, registry, toWire);
                            }
                        }
                    }
                    break;
                default:
                    break;
                }
            }
        }
    }

    Json FieldsToJson(const void* obj, const TypeInfo& type, const TypeRegistry& registry)
    {
        Json out = JsonWriteFields(obj, type, registry, MakeHooks());
        ConvertAssetHandles(out, type, registry, /*toWire=*/true);
        return out;
    }

    VoidResult JsonToFields(const Json& source, void* obj, const TypeInfo& type,
                            const TypeRegistry& registry)
    {
        Json parsed = source;
        ConvertAssetHandles(parsed, type, registry, /*toWire=*/false);
        return JsonReadFields(obj, type, parsed, registry, MakeHooks(),
                              /*allowUnknownFields=*/true);
    }
}
