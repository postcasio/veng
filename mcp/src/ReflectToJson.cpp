#include "ReflectToJson.h"

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Entity.h>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace Veng::Mcp
{
    using Json = nlohmann::json;

    namespace
    {
        /// @brief Reads a trivially-copyable leaf of type T from the field storage.
        template <class T>
        T ReadLeaf(const void* fieldPtr)
        {
            T value{};
            std::memcpy(&value, fieldPtr, sizeof(T));
            return value;
        }

        /// @brief Emits a Scalar leaf as the JSON number/bool matching its C++ type.
        ///
        /// The Scalar FieldClass covers bool / f32 / i32 / u32 / u64; the concrete type is
        /// recovered from the leaf's TypeId so each emits as its natural JSON kind.
        Json ScalarToJson(const void* fieldPtr, TypeId type, const TypeRegistry& registry)
        {
            if (type == registry.IdOf<bool>())
            {
                return ReadLeaf<bool>(fieldPtr);
            }
            if (type == registry.IdOf<f32>())
            {
                return ReadLeaf<f32>(fieldPtr);
            }
            if (type == registry.IdOf<i32>())
            {
                return ReadLeaf<i32>(fieldPtr);
            }
            if (type == registry.IdOf<u32>())
            {
                return ReadLeaf<u32>(fieldPtr);
            }
            if (type == registry.IdOf<u64>())
            {
                return ReadLeaf<u64>(fieldPtr);
            }
            // An unknown Scalar leaf: emit its raw bytes' size as a fallback marker rather
            // than guess a width. The closed leaf set above covers every registered scalar.
            return Json{{"unknownScalar", registry.Info(type).QualifiedName}};
        }

        /// @brief Emits a Vector leaf (vec2/vec3/vec4/uvec2) as a JSON array of components.
        Json VectorToJson(const void* fieldPtr, TypeId type, const TypeRegistry& registry)
        {
            // uvec2 is the one unsigned-integer vector; every other Vector leaf is float.
            if (type == registry.IdOf<uvec2>())
            {
                const auto value = ReadLeaf<uvec2>(fieldPtr);
                return Json::array({value.x, value.y});
            }

            // Float vectors are contiguous f32 components; the leaf's byte size gives the count.
            const usize count = registry.Info(type).Size / sizeof(f32);
            Json out = Json::array();
            for (usize i = 0; i < count; ++i)
            {
                f32 component = 0.0f;
                std::memcpy(&component, static_cast<const u8*>(fieldPtr) + i * sizeof(f32),
                            sizeof(f32));
                out.push_back(component);
            }
            return out;
        }

        /// @brief Emits an Enum leaf as the enumerator name plus the raw backing integer.
        ///
        /// Matches the backing value against the enum's {name, value} table; a value with no
        /// matching enumerator (or an enum authored without VE_ENUM metadata) emits only the
        /// integer.
        Json EnumToJson(const void* fieldPtr, const TypeInfo& info)
        {
            // Enum leaves back onto an integer of the leaf's size; widen to i64 for matching.
            i64 raw = 0;
            std::memcpy(&raw, fieldPtr, std::min(info.Size, sizeof(i64)));

            Json out;
            out["value"] = raw;
            for (const EnumEntry& entry : info.Enumerators)
            {
                if (entry.Value == raw)
                {
                    out["name"] = entry.Name;
                    break;
                }
            }
            return out;
        }

        Json ValueToJson(const void* fieldPtr, const FieldDescriptor& field,
                         const TypeRegistry& registry);

        /// @brief Emits one array element through a synthetic element descriptor.
        Json ElementToJson(const void* elementPtr, TypeId elementType, const TypeRegistry& registry)
        {
            const TypeInfo& element = registry.Info(elementType);
            FieldDescriptor elementDesc;
            elementDesc.Type = elementType;
            elementDesc.Class = element.Class;
            return ValueToJson(elementPtr, elementDesc, registry);
        }

        /// @brief Emits a single field's value as JSON, branching on its FieldClass.
        Json ValueToJson(const void* fieldPtr, const FieldDescriptor& field,
                         const TypeRegistry& registry)
        {
            switch (field.Class)
            {
            case FieldClass::Scalar:
            {
                return ScalarToJson(fieldPtr, field.Type, registry);
            }
            case FieldClass::Vector:
            {
                return VectorToJson(fieldPtr, field.Type, registry);
            }
            case FieldClass::Enum:
            {
                return EnumToJson(fieldPtr, registry.Info(field.Type));
            }
            case FieldClass::Quaternion:
            {
                // A quat is four contiguous f32 (w, x, y, z in glm's storage order).
                Json out = Json::array();
                for (usize i = 0; i < 4; ++i)
                {
                    f32 component = 0.0f;
                    std::memcpy(&component, static_cast<const u8*>(fieldPtr) + i * sizeof(f32),
                                sizeof(f32));
                    out.push_back(component);
                }
                return out;
            }
            case FieldClass::Matrix:
            {
                // A mat4 is 16 contiguous f32, emitted as four rows of four for readability.
                Json out = Json::array();
                for (usize row = 0; row < 4; ++row)
                {
                    Json line = Json::array();
                    for (usize col = 0; col < 4; ++col)
                    {
                        f32 component = 0.0f;
                        std::memcpy(&component,
                                    static_cast<const u8*>(fieldPtr) +
                                        (row * 4 + col) * sizeof(f32),
                                    sizeof(f32));
                        line.push_back(component);
                    }
                    out.push_back(std::move(line));
                }
                return out;
            }
            case FieldClass::String:
            {
                return *static_cast<const string*>(fieldPtr);
            }
            case FieldClass::AssetHandle:
            {
                // The AssetId is the leading u64 of AssetHandle<T> (offset 0, pinned by a
                // static_assert in AssetHandle.h). Emit it as a decimal string so a 64-bit id
                // round-trips exactly (JSON numbers lose precision past 2^53).
                u64 id = 0;
                std::memcpy(&id, fieldPtr, sizeof(id));
                return std::to_string(id);
            }
            case FieldClass::Reference:
            {
                const auto& entity = *static_cast<const Entity*>(fieldPtr);
                return Json{{"index", entity.Index}, {"generation", entity.Generation}};
            }
            case FieldClass::Struct:
            {
                if (!registry.IsRegistered(field.Type))
                {
                    return Json{{"unregisteredType", field.Type}};
                }
                return FieldsToJson(fieldPtr, registry.Info(field.Type), registry);
            }
            case FieldClass::Variant:
            {
                const TypeInfo& info = registry.Info(field.Type);
                const TypeId active = info.VariantActiveType(fieldPtr);
                if (active == InvalidTypeId)
                {
                    return Json(nullptr);
                }
                if (!registry.IsRegistered(active))
                {
                    return Json{{"unregisteredType", active}};
                }
                const TypeInfo& alternative = registry.Info(active);
                const void* memberPtr = info.VariantActivePtrConst(fieldPtr);
                return Json{{"type", alternative.QualifiedName},
                            {"value", FieldsToJson(memberPtr, alternative, registry)}};
            }
            case FieldClass::Array:
            {
                Json out = Json::array();
                if (!registry.IsRegistered(field.ElementType))
                {
                    return out;
                }
                const usize count = field.ArraySize(fieldPtr);
                for (usize i = 0; i < count; ++i)
                {
                    const void* elementPtr = field.ArrayElementConst(fieldPtr, i);
                    out.push_back(ElementToJson(elementPtr, field.ElementType, registry));
                }
                return out;
            }
            }

            return Json(nullptr);
        }
    }

    Json FieldsToJson(const void* obj, const TypeInfo& type, const TypeRegistry& registry)
    {
        Json out = Json::object();
        for (const FieldDescriptor& field : type.Fields)
        {
            const void* fieldPtr = static_cast<const u8*>(obj) + field.Offset;
            out[field.Name] = ValueToJson(fieldPtr, field, registry);
        }
        return out;
    }

    namespace
    {
        /// @brief Upper bound on a JSON-driven array resize.
        ///
        /// A single trusted local client, so this guards against an accidental huge element
        /// count in a malformed request, not a hostile one — it caps the allocation a bad
        /// `values` array can force rather than trusting the incoming size.
        constexpr usize MaxArrayElements = 1u << 20;

        /// @brief Writes a trivially-copyable leaf of type T into the field storage.
        template <class T>
        void WriteLeaf(void* fieldPtr, T value)
        {
            std::memcpy(fieldPtr, &value, sizeof(T));
        }

        /// @brief Parses a JSON number/bool into a Scalar leaf of its concrete C++ type.
        VoidResult ScalarFromJson(const Json& value, void* fieldPtr, TypeId type,
                                  const TypeRegistry& registry)
        {
            if (type == registry.IdOf<bool>())
            {
                if (!value.is_boolean() && !value.is_number())
                {
                    return std::unexpected(string("expected a boolean scalar"));
                }
                WriteLeaf<bool>(fieldPtr, value.get<bool>());
                return {};
            }
            if (!value.is_number())
            {
                return std::unexpected(string("expected a numeric scalar"));
            }
            if (type == registry.IdOf<f32>())
            {
                WriteLeaf<f32>(fieldPtr, value.get<f32>());
                return {};
            }
            if (type == registry.IdOf<i32>())
            {
                WriteLeaf<i32>(fieldPtr, value.get<i32>());
                return {};
            }
            if (type == registry.IdOf<u32>())
            {
                WriteLeaf<u32>(fieldPtr, value.get<u32>());
                return {};
            }
            if (type == registry.IdOf<u64>())
            {
                WriteLeaf<u64>(fieldPtr, value.get<u64>());
                return {};
            }
            return std::unexpected(
                fmt::format("unsupported scalar type '{}'", registry.Info(type).QualifiedName));
        }

        /// @brief Parses a JSON array into a Vector leaf's contiguous components.
        VoidResult VectorFromJson(const Json& value, void* fieldPtr, TypeId type,
                                  const TypeRegistry& registry)
        {
            if (!value.is_array())
            {
                return std::unexpected(string("expected an array of vector components"));
            }

            if (type == registry.IdOf<uvec2>())
            {
                if (value.size() != 2)
                {
                    return std::unexpected(string("uvec2 expects 2 components"));
                }
                WriteLeaf<uvec2>(fieldPtr, uvec2(value[0].get<u32>(), value[1].get<u32>()));
                return {};
            }

            const usize count = registry.Info(type).Size / sizeof(f32);
            if (value.size() != count)
            {
                return std::unexpected(fmt::format("vector expects {} components", count));
            }
            for (usize i = 0; i < count; ++i)
            {
                if (!value[i].is_number())
                {
                    return std::unexpected(string("vector component is not a number"));
                }
                const f32 component = value[i].get<f32>();
                std::memcpy(static_cast<u8*>(fieldPtr) + i * sizeof(f32), &component, sizeof(f32));
            }
            return {};
        }

        /// @brief Parses a JSON enum value (an enumerator name or its raw integer) into an Enum leaf.
        VoidResult EnumFromJson(const Json& value, void* fieldPtr, const TypeInfo& info)
        {
            i64 raw = 0;
            bool resolved = false;

            // Accept the read-side { name, value } echo, a bare enumerator name, or a raw integer.
            const Json selector =
                value.is_object() && value.contains("name") ? value["name"] : value;

            if (selector.is_string())
            {
                const string name = selector.get<string>();
                for (const EnumEntry& entry : info.Enumerators)
                {
                    if (entry.Name == name)
                    {
                        raw = entry.Value;
                        resolved = true;
                        break;
                    }
                }
                if (!resolved)
                {
                    return std::unexpected(fmt::format("unknown enumerator '{}' for enum '{}'",
                                                       name, info.QualifiedName));
                }
            }
            else if (selector.is_number_integer())
            {
                raw = selector.get<i64>();
                resolved = true;
            }
            else if (value.is_object() && value.contains("value") &&
                     value["value"].is_number_integer())
            {
                raw = value["value"].get<i64>();
                resolved = true;
            }

            if (!resolved)
            {
                return std::unexpected(fmt::format(
                    "expected an enumerator name or integer for enum '{}'", info.QualifiedName));
            }
            std::memcpy(fieldPtr, &raw, std::min(info.Size, sizeof(i64)));
            return {};
        }

        VoidResult ValueFromJson(const Json& value, void* fieldPtr, const FieldDescriptor& field,
                                 const TypeRegistry& registry);

        /// @brief Writes one array element through a synthetic element descriptor.
        VoidResult ElementFromJson(const Json& value, void* elementPtr, TypeId elementType,
                                   const TypeRegistry& registry)
        {
            const TypeInfo& element = registry.Info(elementType);
            FieldDescriptor elementDesc;
            elementDesc.Type = elementType;
            elementDesc.Class = element.Class;
            return ValueFromJson(value, elementPtr, elementDesc, registry);
        }

        /// @brief Parses a single JSON value into a field's storage, branching on its FieldClass.
        VoidResult ValueFromJson(const Json& value, void* fieldPtr, const FieldDescriptor& field,
                                 const TypeRegistry& registry)
        {
            switch (field.Class)
            {
            case FieldClass::Scalar:
            {
                return ScalarFromJson(value, fieldPtr, field.Type, registry);
            }
            case FieldClass::Vector:
            {
                return VectorFromJson(value, fieldPtr, field.Type, registry);
            }
            case FieldClass::Enum:
            {
                return EnumFromJson(value, fieldPtr, registry.Info(field.Type));
            }
            case FieldClass::Quaternion:
            {
                if (!value.is_array() || value.size() != 4)
                {
                    return std::unexpected(string("quaternion expects an array of 4 components"));
                }
                for (usize i = 0; i < 4; ++i)
                {
                    if (!value[i].is_number())
                    {
                        return std::unexpected(string("quaternion component is not a number"));
                    }
                    const f32 component = value[i].get<f32>();
                    std::memcpy(static_cast<u8*>(fieldPtr) + i * sizeof(f32), &component,
                                sizeof(f32));
                }
                return {};
            }
            case FieldClass::Matrix:
            {
                // A mat4 is emitted as four rows of four; accept the same shape.
                if (!value.is_array() || value.size() != 4)
                {
                    return std::unexpected(string("matrix expects 4 rows of 4"));
                }
                for (usize row = 0; row < 4; ++row)
                {
                    if (!value[row].is_array() || value[row].size() != 4)
                    {
                        return std::unexpected(string("matrix row expects 4 components"));
                    }
                    for (usize col = 0; col < 4; ++col)
                    {
                        if (!value[row][col].is_number())
                        {
                            return std::unexpected(string("matrix component is not a number"));
                        }
                        const f32 component = value[row][col].get<f32>();
                        std::memcpy(static_cast<u8*>(fieldPtr) + (row * 4 + col) * sizeof(f32),
                                    &component, sizeof(f32));
                    }
                }
                return {};
            }
            case FieldClass::String:
            {
                if (!value.is_string())
                {
                    return std::unexpected(string("expected a string"));
                }
                *static_cast<string*>(fieldPtr) = value.get<string>();
                return {};
            }
            case FieldClass::AssetHandle:
            {
                // The AssetId is the leading u64 of AssetHandle<T> (offset 0). Accept a decimal
                // string (the read-side form, exact past 2^53) or a JSON number.
                u64 id = 0;
                if (value.is_string())
                {
                    const string text = value.get<string>();
                    if (std::from_chars(text.data(), text.data() + text.size(), id).ec !=
                        std::errc{})
                    {
                        return std::unexpected(fmt::format("invalid AssetId '{}'", text));
                    }
                }
                else if (value.is_number_unsigned())
                {
                    id = value.get<u64>();
                }
                else if (value.is_null())
                {
                    id = 0;
                }
                else
                {
                    return std::unexpected(
                        string("expected an AssetId as a decimal string or number"));
                }
                std::memcpy(fieldPtr, &id, sizeof(id));
                return {};
            }
            case FieldClass::Reference:
            {
                if (!value.is_object() || !value.contains("index"))
                {
                    return std::unexpected(
                        string("entity reference expects { index, generation }"));
                }
                auto& entity = *static_cast<Entity*>(fieldPtr);
                entity.Index = value["index"].get<u32>();
                entity.Generation =
                    value.contains("generation") ? value["generation"].get<u32>() : 0;
                return {};
            }
            case FieldClass::Struct:
            {
                if (!value.is_object())
                {
                    return std::unexpected(string("expected an object for a struct field"));
                }
                if (!registry.IsRegistered(field.Type))
                {
                    return std::unexpected(string("struct field names an unregistered type"));
                }
                return JsonToFields(value, fieldPtr, registry.Info(field.Type), registry);
            }
            case FieldClass::Variant:
            {
                const TypeInfo& info = registry.Info(field.Type);
                if (value.is_null())
                {
                    info.VariantClear(fieldPtr);
                    return {};
                }
                if (!value.is_object() || !value.contains("type"))
                {
                    return std::unexpected(string("variant expects { type, value }"));
                }
                const string typeName = value["type"].get<string>();
                TypeId active = InvalidTypeId;
                for (const TypeId alternative : info.VariantAlternatives)
                {
                    if (registry.IsRegistered(alternative) &&
                        TypeNameMatches(registry.Info(alternative), typeName))
                    {
                        active = alternative;
                        break;
                    }
                }
                if (active == InvalidTypeId)
                {
                    return std::unexpected(fmt::format("'{}' is not an alternative of variant '{}'",
                                                       typeName, info.QualifiedName));
                }
                void* memberPtr = info.VariantSetActive(fieldPtr, active);
                if (memberPtr == nullptr)
                {
                    return std::unexpected(
                        fmt::format("failed to activate variant alternative '{}'", typeName));
                }
                const Json& inner = value.contains("value") ? value["value"] : Json::object();
                if (!inner.is_object())
                {
                    return std::unexpected(string("variant 'value' must be an object"));
                }
                return JsonToFields(inner, memberPtr, registry.Info(active), registry);
            }
            case FieldClass::Array:
            {
                if (!value.is_array())
                {
                    return std::unexpected(string("expected an array"));
                }
                if (!registry.IsRegistered(field.ElementType))
                {
                    return std::unexpected(
                        string("array field names an unregistered element type"));
                }
                if (value.size() > MaxArrayElements)
                {
                    return std::unexpected(fmt::format("array of {} elements exceeds the cap of {}",
                                                       value.size(), MaxArrayElements));
                }
                field.ArrayResize(fieldPtr, value.size());
                for (usize i = 0; i < value.size(); ++i)
                {
                    void* elementPtr = field.ArrayElement(fieldPtr, i);
                    const VoidResult result =
                        ElementFromJson(value[i], elementPtr, field.ElementType, registry);
                    if (!result)
                    {
                        return std::unexpected(fmt::format("[{}]: {}", i, result.error()));
                    }
                }
                return {};
            }
            }

            return {};
        }
    }

    VoidResult JsonToFields(const Json& source, void* obj, const TypeInfo& type,
                            const TypeRegistry& registry)
    {
        if (!source.is_object())
        {
            return std::unexpected(string("expected a JSON object of field values"));
        }
        for (const FieldDescriptor& field : type.Fields)
        {
            const auto it = source.find(field.Name);
            if (it == source.end())
            {
                // Schema-drift tolerance: a field the source omits keeps its current value.
                continue;
            }
            void* fieldPtr = static_cast<u8*>(obj) + field.Offset;
            const VoidResult result = ValueFromJson(*it, fieldPtr, field, registry);
            if (!result)
            {
                return std::unexpected(fmt::format("field '{}': {}", field.Name, result.error()));
            }
        }
        return {};
    }
}
