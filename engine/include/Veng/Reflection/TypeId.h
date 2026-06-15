#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>

#include <Veng/Asset/AssetHandle.h>

#include <type_traits>

namespace Veng
{
    class Texture;
    class Mesh;
    class Material;

    // A stable, authored type identifier — the AssetId discipline applied to
    // C++ types. Engine builtins carry a hardcoded 0x…ULL literal checked into
    // the source; game types mint their own with `vengc generate-id`. Because
    // the id is a literal, not a compiler type-hash, it is a compile-time
    // constant and byte-identical across the eventual dlopen boundary.
    using TypeId = u64;

    // 0 is reserved as the invalid id; every minted id is non-zero.
    inline constexpr TypeId InvalidTypeId = 0;

    // The closed meta-kind a generic walker switches on. Unlike the open TypeId
    // space (any game type adds a new id with no engine change), this set is
    // fixed: the serializer and the future editor inspector share exactly these
    // cases. Reference is an intra-scene Entity reference the future loader
    // remaps; Struct recurses into the field type's own Fields.
    enum class FieldClass : u8
    {
        Scalar,
        Vector,
        Quaternion,
        Matrix,
        String,
        AssetHandle,
        Reference,
        Struct,
        Enum,
    };

    // A type declares its stable TypeId (and, via VE_REFLECT, its fields) by
    // specialising this trait. VE_TYPE writes an id-only specialisation;
    // VE_REFLECT writes a fielded one carrying Id + Name() + Class() + Fields().
    // TypeRegistry::Register reads it; the reflection layer enriches the same
    // trait. Leaf field types carry their identity on ReflectLeaf<T> instead.
    template <class T>
    struct VengReflect;

    // A leaf C++ type names its stable TypeId + FieldClass through this trait.
    // The engine pre-specialises it for the builtin leaves below; a game adds a
    // new leaf by specialising it for its own type with a `vengc generate-id`
    // value — no engine change. Structs (FieldClass::Struct) carry their identity
    // on VengReflect<T> instead, written by VE_REFLECT.
    template <class T>
    struct ReflectLeaf;

    namespace Detail
    {
        // True when T has a leaf trait (a Scalar/Vector/.../AssetHandle/Reference
        // leaf), false when T is a reflected struct carrying VengReflect<T>.
        template <class T, class = void>
        inline constexpr bool IsReflectLeaf = false;

        template <class T>
        inline constexpr bool IsReflectLeaf<T, std::void_t<decltype(ReflectLeaf<T>::Id)>> = true;
    }

    // The stable TypeId of any reflected type, resolved at compile time: a leaf
    // reads ReflectLeaf<T>::Id, a struct reads VengReflect<T>::Id. There is no
    // registration-ordering burden — the id is a constant, independent of the
    // registry.
    template <class T>
    constexpr TypeId TypeIdOf()
    {
        if constexpr (Detail::IsReflectLeaf<T>)
            return ReflectLeaf<T>::Id;
        else
            return VengReflect<T>::Id;
    }

    // The FieldClass of any reflected type, resolved at compile time. A struct is
    // always FieldClass::Struct.
    template <class T>
    constexpr FieldClass FieldClassOf()
    {
        if constexpr (Detail::IsReflectLeaf<T>)
            return ReflectLeaf<T>::Class;
        else
            return FieldClass::Struct;
    }

    // ----- Builtin leaf traits -------------------------------------------------
    // Each carries a hardcoded TypeId literal checked into the engine, exactly
    // like the core pack's built-in asset ids, plus its meta-kind. Distinct
    // C++ types that share a representation (bool vs u8, etc.) still get distinct
    // ids — identity is per C++ type, not per byte layout.

    template <> struct ReflectLeaf<bool>
    { static constexpr TypeId Id = 0x283EDB5B266A27EDULL; static constexpr FieldClass Class = FieldClass::Scalar; };

    template <> struct ReflectLeaf<f32>
    { static constexpr TypeId Id = 0x4AF0D89664A476FBULL; static constexpr FieldClass Class = FieldClass::Scalar; };

    template <> struct ReflectLeaf<i32>
    { static constexpr TypeId Id = 0xE4A543818EB46182ULL; static constexpr FieldClass Class = FieldClass::Scalar; };

    template <> struct ReflectLeaf<u32>
    { static constexpr TypeId Id = 0x6AD25BC2BE1A5D65ULL; static constexpr FieldClass Class = FieldClass::Scalar; };

    template <> struct ReflectLeaf<u64>
    { static constexpr TypeId Id = 0x94AB42FEF4E32D87ULL; static constexpr FieldClass Class = FieldClass::Scalar; };

    template <> struct ReflectLeaf<vec2>
    { static constexpr TypeId Id = 0xB9A6A5F871901160ULL; static constexpr FieldClass Class = FieldClass::Vector; };

    template <> struct ReflectLeaf<vec3>
    { static constexpr TypeId Id = 0xA9A78263CAA293E7ULL; static constexpr FieldClass Class = FieldClass::Vector; };

    template <> struct ReflectLeaf<vec4>
    { static constexpr TypeId Id = 0xA936BFC80085F684ULL; static constexpr FieldClass Class = FieldClass::Vector; };

    template <> struct ReflectLeaf<quat>
    { static constexpr TypeId Id = 0xFD92495C91720213ULL; static constexpr FieldClass Class = FieldClass::Quaternion; };

    template <> struct ReflectLeaf<mat4>
    { static constexpr TypeId Id = 0x8ABB4818B9CC633EULL; static constexpr FieldClass Class = FieldClass::Matrix; };

    template <> struct ReflectLeaf<string>
    { static constexpr TypeId Id = 0x2E46B7DE1FFC7DFCULL; static constexpr FieldClass Class = FieldClass::String; };

    template <> struct ReflectLeaf<AssetHandle<Texture>>
    { static constexpr TypeId Id = 0x612EE7E69BE7B848ULL; static constexpr FieldClass Class = FieldClass::AssetHandle; };

    template <> struct ReflectLeaf<AssetHandle<Mesh>>
    { static constexpr TypeId Id = 0x1CD2C85C50AFC9E0ULL; static constexpr FieldClass Class = FieldClass::AssetHandle; };

    template <> struct ReflectLeaf<AssetHandle<Material>>
    { static constexpr TypeId Id = 0x3992D11EB4362B4CULL; static constexpr FieldClass Class = FieldClass::AssetHandle; };

    // Entity is an intra-scene reference, not a value leaf — the future loader
    // recognises FieldClass::Reference to remap it into a fresh Scene.
    template <> struct ReflectLeaf<Entity>
    { static constexpr TypeId Id = 0x448BDF481E27075EULL; static constexpr FieldClass Class = FieldClass::Reference; };
}
