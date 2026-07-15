#include <Veng/Reflection/JsonSerialize.h>

#include <Veng/Assert.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Reflection/EnumName.h>

#include <algorithm>
#include <charconv>
#include <cstring>

#include <fmt/format.h>

namespace Veng
{
    namespace
    {
        using Json = nlohmann::json;

        VoidResult ReadValue(void* fieldPtr, const FieldDescriptor& field, const Json& value,
                             const TypeRegistry& registry, const JsonFieldHooks& hooks,
                             bool allowUnknownFields, const string& path);

        VoidResult ReadFieldsAt(void* obj, const TypeInfo& type, const Json& value,
                                const TypeRegistry& registry, const JsonFieldHooks& hooks,
                                bool allowUnknownFields, const string& path);

        Json WriteValue(const void* fieldPtr, const FieldDescriptor& field,
                        const TypeRegistry& registry, const JsonFieldHooks& hooks);

        // Appends ".name" (or "name" at the root) to a dotted field path.
        string Descend(const string& path, string_view name)
        {
            return path.empty() ? string(name) : fmt::format("{}.{}", path, name);
        }

        // ---- Read side ------------------------------------------------------

        // Reads a 64-bit integer leaf from either a JSON string (the lossless form — a value past
        // 2^53 truncates through JSON's IEEE-754-double number) or a plain JSON number (accepted for
        // small values and hand-authored convenience). A string is parsed base-10, sign included, or
        // base-16 when it carries a `0x`/`0X` prefix (the canonical spelling for a minted id).
        template <typename Int>
        Result<Int> ReadInteger64(const Json& value, const string& path)
        {
            if (value.is_string())
            {
                const string text = value.get<string>();
                const char* begin = text.data();
                const char* const end = text.data() + text.size();
                if (begin != end && *begin == '+')
                {
                    ++begin;
                }
                int base = 10;
                if (end - begin >= 2 && begin[0] == '0' && (begin[1] == 'x' || begin[1] == 'X'))
                {
                    begin += 2;
                    base = 16;
                }
                Int parsed = 0;
                const auto [ptr, ec] = std::from_chars(begin, end, parsed, base);
                if (ec != std::errc{} || ptr != end)
                {
                    return std::unexpected(
                        fmt::format("{}: '{}' is not a valid 64-bit integer", path, text));
                }
                return parsed;
            }
            if (value.is_number_integer() || value.is_number_unsigned())
            {
                return value.get<Int>();
            }
            return std::unexpected(
                fmt::format("{}: expected a 64-bit integer as a number or string", path));
        }

        VoidResult ReadScalar(void* fieldPtr, TypeId type, const Json& value, const string& path)
        {
            // 64-bit integers accept a JSON string (lossless past 2^53) as well as a number, so they
            // are dispatched before the number/boolean guard the narrower scalars require.
            if (type == TypeIdOf<i64>())
            {
                const Result<i64> v = ReadInteger64<i64>(value, path);
                if (!v)
                {
                    return std::unexpected(v.error());
                }
                std::memcpy(fieldPtr, &*v, sizeof(i64));
                return {};
            }
            if (type == TypeIdOf<u64>())
            {
                const Result<u64> v = ReadInteger64<u64>(value, path);
                if (!v)
                {
                    return std::unexpected(v.error());
                }
                std::memcpy(fieldPtr, &*v, sizeof(u64));
                return {};
            }
            // A non-builtin scalar leaf is a 64-bit id-family enum (ActionId, GuiDriverId, …): read
            // it as a u64 (a string or number), so it authors as a hex-id string exactly like a
            // minted id. Handled before the number/boolean guard the narrower scalars require.
            if (type != TypeIdOf<bool>() && type != TypeIdOf<f32>() && type != TypeIdOf<i32>() &&
                type != TypeIdOf<u32>())
            {
                const Result<u64> v = ReadInteger64<u64>(value, path);
                if (!v)
                {
                    return std::unexpected(v.error());
                }
                std::memcpy(fieldPtr, &*v, sizeof(u64));
                return {};
            }

            if (!value.is_number() && !value.is_boolean())
            {
                return std::unexpected(fmt::format("{}: expected a number or boolean", path));
            }

            if (type == TypeIdOf<bool>())
            {
                const bool v = value.is_boolean() ? value.get<bool>() : (value.get<f64>() != 0.0);
                std::memcpy(fieldPtr, &v, sizeof(v));
                return {};
            }
            if (type == TypeIdOf<f32>())
            {
                const f32 v = value.get<f32>();
                std::memcpy(fieldPtr, &v, sizeof(v));
                return {};
            }
            if (type == TypeIdOf<i32>())
            {
                const i32 v = value.get<i32>();
                std::memcpy(fieldPtr, &v, sizeof(v));
                return {};
            }
            if (type == TypeIdOf<u32>())
            {
                const u32 v = value.get<u32>();
                std::memcpy(fieldPtr, &v, sizeof(v));
                return {};
            }
            return std::unexpected(fmt::format("{}: unsupported scalar leaf type", path));
        }

        VoidResult ReadVectorLike(void* fieldPtr, TypeId type, const Json& value,
                                  const TypeRegistry& registry, const string& path)
        {
            // Components are stored in the field's storage type — f32 for a float
            // vector/quat/matrix, u32 for an unsigned-integer vector. A quat is
            // [x,y,z,w] (glm memory layout); a mat4 flattens the same as a
            // 16-component vector, so this one arm covers Vector/Quaternion/Matrix.
            const bool unsignedVector = type == TypeIdOf<uvec2>();
            const usize componentSize = unsignedVector ? sizeof(u32) : sizeof(f32);
            const usize size = registry.Info(type).Size;
            const usize arity = size / componentSize;

            if (!value.is_array() || value.size() != arity)
            {
                return std::unexpected(
                    fmt::format("{}: expected an array of {} numbers", path, arity));
            }
            for (const Json& elem : value)
            {
                if (!elem.is_number())
                {
                    return std::unexpected(
                        fmt::format("{}: array contains a non-number element", path));
                }
            }

            if (unsignedVector)
            {
                vector<u32> components;
                components.reserve(arity);
                for (const Json& elem : value)
                {
                    components.push_back(elem.get<u32>());
                }
                std::memcpy(fieldPtr, components.data(), arity * sizeof(u32));
            }
            else
            {
                vector<f32> components;
                components.reserve(arity);
                for (const Json& elem : value)
                {
                    components.push_back(elem.get<f32>());
                }
                std::memcpy(fieldPtr, components.data(), arity * sizeof(f32));
            }
            return {};
        }

        VoidResult ReadAssetHandle(void* fieldPtr, const Json& value, const JsonFieldHooks& hooks,
                                   TypeId fieldType, const string& path)
        {
            if (value.is_null())
            {
                const u64 id = 0;
                std::memcpy(fieldPtr, &id, sizeof(id));
                return {};
            }
            if (!value.is_string())
            {
                return std::unexpected(fmt::format("{}: expected a hex-string AssetId", path));
            }

            const optional<AssetId> parsed = ParseAssetId(value.get<string>());
            if (!parsed)
            {
                return std::unexpected(fmt::format("{}: expected a hex-string AssetId", path));
            }
            const u64 id = parsed->Value;
            if (id != 0 && hooks.ValidateAssetId)
            {
                const VoidResult validated = hooks.ValidateAssetId(id, fieldType);
                if (!validated)
                {
                    return std::unexpected(fmt::format("{}: {}", path, validated.error()));
                }
            }

            // The handle stores the AssetId at offset 0 (pinned in AssetHandle.h).
            std::memcpy(fieldPtr, &id, sizeof(id));
            return {};
        }

        VoidResult ReadEnum(void* fieldPtr, const TypeInfo& info, const Json& value,
                            const string& path)
        {
            if (!value.is_string())
            {
                return std::unexpected(fmt::format("{}: expected an enumerator name", path));
            }
            const optional<i64> parsed = ParseEnumValue(info, value.get<string>());
            if (!parsed)
            {
                return std::unexpected(
                    fmt::format("{}: unknown enumerator '{}'", path, value.get<string>()));
            }
            StoreEnumBits(fieldPtr, info, *parsed);
            return {};
        }

        VoidResult ReadReference(void* fieldPtr, const Json& value, const JsonFieldHooks& hooks,
                                 const string& path)
        {
            auto& entity = *static_cast<Entity*>(fieldPtr);
            if (value.is_null())
            {
                entity = Entity::Null;
                return {};
            }
            if (!hooks.ReadReference)
            {
                return std::unexpected(
                    fmt::format("{}: Reference fields require a ReadReference hook", path));
            }
            const Result<Entity> resolved = hooks.ReadReference(value);
            if (!resolved)
            {
                return std::unexpected(fmt::format("{}: {}", path, resolved.error()));
            }
            entity = *resolved;
            return {};
        }

        VoidResult ReadStruct(void* fieldPtr, TypeId fieldType, const Json& value,
                              const TypeRegistry& registry, const JsonFieldHooks& hooks,
                              bool allowUnknownFields, const string& path)
        {
            if (!value.is_object())
            {
                return std::unexpected(fmt::format("{}: expected an object", path));
            }
            return ReadFieldsAt(fieldPtr, registry.Info(fieldType), value, registry, hooks,
                                allowUnknownFields, path);
        }

        // Matches `name` (bare or namespace-qualified) against each of the variant's
        // alternatives, returning the matched TypeId or InvalidTypeId.
        TypeId MatchAlternativeByName(const TypeInfo& variant, const string& name,
                                      const TypeRegistry& registry)
        {
            for (const TypeId altId : variant.VariantAlternatives)
            {
                if (TypeNameMatches(registry.Info(altId), name))
                {
                    return altId;
                }
            }
            return InvalidTypeId;
        }

        VoidResult ReadVariant(void* fieldPtr, TypeId fieldType, const Json& value,
                               const TypeRegistry& registry, const JsonFieldHooks& hooks,
                               bool allowUnknownFields, const string& path)
        {
            const TypeInfo& info = registry.Info(fieldType);

            // Null (or absent, handled by the caller) leaves the variant empty.
            if (value.is_null())
            {
                info.VariantClear(fieldPtr);
                return {};
            }

            if (!value.is_object() || !value.contains("type") || !value["type"].is_string())
            {
                return std::unexpected(
                    fmt::format("{}: expected an object with a string 'type' key", path));
            }

            const string typeName = value["type"].get<string>();
            if (typeName.empty())
            {
                info.VariantClear(fieldPtr);
                return {};
            }

            const TypeId chosen = MatchAlternativeByName(info, typeName, registry);
            if (chosen == InvalidTypeId)
            {
                return std::unexpected(fmt::format("{}: '{}' is not an alternative of variant '{}'",
                                                   path, typeName, info.Name));
            }

            void* memberPtr = info.VariantSetActive(fieldPtr, chosen);
            // chosen came from this variant's alternative list, so SetActive succeeds.

            if (value.contains("value"))
            {
                const Json& inner = value["value"];
                if (!inner.is_object())
                {
                    return std::unexpected(
                        fmt::format("{}: variant 'value' must be an object", path));
                }
                return ReadFieldsAt(memberPtr, registry.Info(chosen), inner, registry, hooks,
                                    allowUnknownFields, Descend(path, "value"));
            }
            return {};
        }

        VoidResult ReadArray(void* fieldPtr, const FieldDescriptor& field, const Json& value,
                             const TypeRegistry& registry, const JsonFieldHooks& hooks,
                             bool allowUnknownFields, const string& path)
        {
            if (!value.is_array())
            {
                return std::unexpected(fmt::format("{}: expected an array", path));
            }

            const TypeInfo& element = registry.Info(field.ElementType);

            field.ArrayResize(fieldPtr, value.size());
            for (usize i = 0; i < value.size(); ++i)
            {
                FieldDescriptor elementDesc;
                elementDesc.Name = field.Name;
                elementDesc.Type = field.ElementType;
                elementDesc.Class = element.Class;
                elementDesc.Offset = 0;
                elementDesc.ElementType = InvalidTypeId;

                void* elementPtr = field.ArrayElement(fieldPtr, i);
                const VoidResult bound =
                    ReadValue(elementPtr, elementDesc, value[i], registry, hooks,
                              allowUnknownFields, fmt::format("{}[{}]", path, i));
                if (!bound)
                {
                    return bound;
                }
            }
            return {};
        }

        VoidResult ReadValue(void* fieldPtr, const FieldDescriptor& field, const Json& value,
                             const TypeRegistry& registry, const JsonFieldHooks& hooks,
                             bool allowUnknownFields, const string& path)
        {
            switch (field.Class)
            {
            case FieldClass::Scalar:
                return ReadScalar(fieldPtr, field.Type, value, path);
            case FieldClass::Vector:
            case FieldClass::Quaternion:
            case FieldClass::Matrix:
                return ReadVectorLike(fieldPtr, field.Type, value, registry, path);
            case FieldClass::String:
                if (!value.is_string())
                {
                    return std::unexpected(fmt::format("{}: expected a string", path));
                }
                *static_cast<string*>(fieldPtr) = value.get<string>();
                return {};
            case FieldClass::AssetHandle:
                return ReadAssetHandle(fieldPtr, value, hooks, field.Type, path);
            case FieldClass::Enum:
                return ReadEnum(fieldPtr, registry.Info(field.Type), value, path);
            case FieldClass::Reference:
                return ReadReference(fieldPtr, value, hooks, path);
            case FieldClass::Struct:
                return ReadStruct(fieldPtr, field.Type, value, registry, hooks, allowUnknownFields,
                                  path);
            case FieldClass::Variant:
                return ReadVariant(fieldPtr, field.Type, value, registry, hooks, allowUnknownFields,
                                   path);
            case FieldClass::Array:
                return ReadArray(fieldPtr, field, value, registry, hooks, allowUnknownFields, path);
            }
            return std::unexpected(fmt::format("{}: unhandled field class", path));
        }

        // ---- Write side ------------------------------------------------------

        Json WriteScalar(const void* fieldPtr, TypeId type)
        {
            if (type == TypeIdOf<bool>())
            {
                bool v = false;
                std::memcpy(&v, fieldPtr, sizeof(v));
                return v;
            }
            if (type == TypeIdOf<f32>())
            {
                f32 v = 0.0f;
                std::memcpy(&v, fieldPtr, sizeof(v));
                return v;
            }
            if (type == TypeIdOf<i32>())
            {
                i32 v = 0;
                std::memcpy(&v, fieldPtr, sizeof(v));
                return v;
            }
            if (type == TypeIdOf<u32>())
            {
                u32 v = 0;
                std::memcpy(&v, fieldPtr, sizeof(v));
                return v;
            }
            // 64-bit integers serialize as a decimal string: a bare JSON number past 2^53 truncates
            // through the IEEE-754-double number pipeline, so the string is the lossless form (the
            // same reason minted ids are stored as strings). ReadScalar accepts either on the way in.
            if (type == TypeIdOf<i64>())
            {
                i64 v = 0;
                std::memcpy(&v, fieldPtr, sizeof(v));
                return std::to_string(v);
            }
            if (type == TypeIdOf<u64>())
            {
                u64 v = 0;
                std::memcpy(&v, fieldPtr, sizeof(v));
                return std::to_string(v);
            }
            // A non-builtin scalar leaf is a 64-bit id-family enum (ActionId, GuiDriverId, …):
            // serialize it as its canonical hex-id string, the same spelling ReadScalar accepts.
            u64 id = 0;
            std::memcpy(&id, fieldPtr, sizeof(id));
            return fmt::format("0x{:016X}", id);
        }

        Json WriteVectorLike(const void* fieldPtr, TypeId type, const TypeRegistry& registry)
        {
            const bool unsignedVector = type == TypeIdOf<uvec2>();
            const usize componentSize = unsignedVector ? sizeof(u32) : sizeof(f32);
            const usize size = registry.Info(type).Size;
            const usize arity = size / componentSize;

            Json out = Json::array();
            const auto* bytes = static_cast<const u8*>(fieldPtr);
            for (usize i = 0; i < arity; ++i)
            {
                if (unsignedVector)
                {
                    u32 v = 0;
                    std::memcpy(&v, bytes + i * sizeof(u32), sizeof(v));
                    out.push_back(v);
                }
                else
                {
                    f32 v = 0.0f;
                    std::memcpy(&v, bytes + i * sizeof(f32), sizeof(v));
                    out.push_back(v);
                }
            }
            return out;
        }

        Json WriteAssetHandle(const void* fieldPtr)
        {
            u64 id = 0;
            std::memcpy(&id, fieldPtr, sizeof(id));
            return id == 0 ? Json(nullptr) : Json(FormatAssetId(AssetId{id}));
        }

        Json WriteEnum(const void* fieldPtr, const TypeInfo& info)
        {
            return EnumeratorName(info, LoadEnumBits(fieldPtr, info));
        }

        Json WriteReference(const void* fieldPtr, const JsonFieldHooks& hooks)
        {
            const auto& entity = *static_cast<const Entity*>(fieldPtr);
            if (entity.IsNull())
            {
                return nullptr;
            }
            VE_ASSERT(static_cast<bool>(hooks.WriteReference),
                      "JsonWriteFields: a Reference field requires a WriteReference hook");
            return hooks.WriteReference(entity);
        }

        Json WriteStruct(const void* fieldPtr, TypeId fieldType, const TypeRegistry& registry,
                         const JsonFieldHooks& hooks)
        {
            return JsonWriteFields(fieldPtr, registry.Info(fieldType), registry, hooks);
        }

        Json WriteVariant(const void* fieldPtr, TypeId fieldType, const TypeRegistry& registry,
                          const JsonFieldHooks& hooks)
        {
            const TypeInfo& info = registry.Info(fieldType);
            const TypeId active = info.VariantActiveType(fieldPtr);
            if (active == InvalidTypeId)
            {
                return nullptr;
            }
            const TypeInfo& alt = registry.Info(active);
            const void* member = info.VariantActivePtrConst(fieldPtr);

            Json out = Json::object();
            out["type"] = alt.QualifiedName;
            out["value"] = JsonWriteFields(member, alt, registry, hooks);
            return out;
        }

        Json WriteArray(const void* fieldPtr, const FieldDescriptor& field,
                        const TypeRegistry& registry, const JsonFieldHooks& hooks)
        {
            const usize count = field.ArraySize(fieldPtr);
            const TypeInfo& element = registry.Info(field.ElementType);

            Json out = Json::array();
            for (usize i = 0; i < count; ++i)
            {
                FieldDescriptor elementDesc;
                elementDesc.Name = field.Name;
                elementDesc.Type = field.ElementType;
                elementDesc.Class = element.Class;
                elementDesc.Offset = 0;
                elementDesc.ElementType = InvalidTypeId;

                const void* elementPtr = field.ArrayElementConst(fieldPtr, i);
                out.push_back(WriteValue(elementPtr, elementDesc, registry, hooks));
            }
            return out;
        }

        Json WriteValue(const void* fieldPtr, const FieldDescriptor& field,
                        const TypeRegistry& registry, const JsonFieldHooks& hooks)
        {
            switch (field.Class)
            {
            case FieldClass::Scalar:
                return WriteScalar(fieldPtr, field.Type);
            case FieldClass::Vector:
            case FieldClass::Quaternion:
            case FieldClass::Matrix:
                return WriteVectorLike(fieldPtr, field.Type, registry);
            case FieldClass::String:
                return *static_cast<const string*>(fieldPtr);
            case FieldClass::AssetHandle:
                return WriteAssetHandle(fieldPtr);
            case FieldClass::Enum:
                return WriteEnum(fieldPtr, registry.Info(field.Type));
            case FieldClass::Reference:
                return WriteReference(fieldPtr, hooks);
            case FieldClass::Struct:
                return WriteStruct(fieldPtr, field.Type, registry, hooks);
            case FieldClass::Variant:
                return WriteVariant(fieldPtr, field.Type, registry, hooks);
            case FieldClass::Array:
                return WriteArray(fieldPtr, field, registry, hooks);
            }
            return nullptr;
        }

        // The internal object-walk, threading the dotted path prefix through struct/variant
        // recursion; JsonReadFields (the public entry point) calls it with an empty prefix.
        VoidResult ReadFieldsAt(void* obj, const TypeInfo& type, const Json& value,
                                const TypeRegistry& registry, const JsonFieldHooks& hooks,
                                bool allowUnknownFields, const string& path)
        {
            if (!value.is_object())
            {
                return std::unexpected(fmt::format("{}: expected a JSON object of field values",
                                                   path.empty() ? "<root>" : path));
            }

            if (!allowUnknownFields)
            {
                for (auto it = value.begin(); it != value.end(); ++it)
                {
                    const bool known =
                        std::ranges::any_of(type.Fields, [&](const FieldDescriptor& field)
                                            { return field.Name == it.key(); });
                    if (!known)
                    {
                        return std::unexpected(
                            fmt::format("{}: unknown field", Descend(path, it.key())));
                    }
                }
            }

            for (const FieldDescriptor& field : type.Fields)
            {
                const auto it = value.find(field.Name);
                if (it == value.end())
                {
                    // Schema-drift tolerance: an omitted field keeps its current value.
                    continue;
                }

                void* fieldPtr = static_cast<u8*>(obj) + field.Offset;
                const VoidResult bound = ReadValue(fieldPtr, field, *it, registry, hooks,
                                                   allowUnknownFields, Descend(path, field.Name));
                if (!bound)
                {
                    return bound;
                }
            }
            return {};
        }
    }

    VoidResult JsonReadFields(void* obj, const TypeInfo& type, const nlohmann::json& value,
                              const TypeRegistry& registry, const JsonFieldHooks& hooks,
                              bool allowUnknownFields)
    {
        return ReadFieldsAt(obj, type, value, registry, hooks, allowUnknownFields, string{});
    }

    nlohmann::json JsonWriteFields(const void* obj, const TypeInfo& type,
                                   const TypeRegistry& registry, const JsonFieldHooks& hooks)
    {
        nlohmann::json out = nlohmann::json::object();
        JsonWriteFields(out, obj, type, registry, hooks);
        return out;
    }

    void JsonWriteFields(nlohmann::json& into, const void* obj, const TypeInfo& type,
                         const TypeRegistry& registry, const JsonFieldHooks& hooks)
    {
        if (!into.is_object())
        {
            into = nlohmann::json::object();
        }

        for (const FieldDescriptor& field : type.Fields)
        {
            const void* fieldPtr = static_cast<const u8*>(obj) + field.Offset;

            // An empty variant (no active alternative) is omitted rather than written as
            // null: the read side leaves an absent variant field empty, and a present
            // null would be rejected by ReadVariant's "expected an object" check.
            if (field.Class == FieldClass::Variant)
            {
                const TypeInfo& variant = registry.Info(field.Type);
                if (variant.VariantActiveType(fieldPtr) == InvalidTypeId)
                {
                    into.erase(field.Name);
                    continue;
                }
            }

            into[field.Name] = WriteValue(fieldPtr, field, registry, hooks);
        }
    }
}
