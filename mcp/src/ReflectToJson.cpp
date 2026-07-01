#include "ReflectToJson.h"

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Entity.h>

#include <algorithm>
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
}
