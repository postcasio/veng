#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>

#include <Veng/Asset/AssetHandle.h>

#include <Veng/Reflection/ReflectionTypes.h>
#include <Veng/Reflection/FieldDescriptor.h>

namespace Veng
{
    class Texture;
    class Mesh;
    class Material;
    class MaterialInstance;
    class Prefab;
    struct Animation;
    class Environment;
    class TypeRegistry;

    /// @brief Identity trait for every reflected type.
    ///
    /// Every reflected type — a builtin scalar, a glm vector, an enum, an
    /// AssetHandle, an Entity reference, a fieldless component, or a fielded
    /// struct — specialises this trait with a uniform member set:
    ///
    ///   static constexpr TypeId Id;                  // authored 0x…ULL
    ///   static constexpr FieldClass Class;           // the type's meta-kind
    ///   static string Name();                        // logs/editor display
    ///   static vector<FieldDescriptor> Fields();     // {} for a leaf / id-only
    ///   static void RegisterDependencies(TypeRegistry&); // no-op for those
    ///
    /// Three authoring macros emit that set: VE_LEAF (a non-struct leaf),
    /// VE_TYPE (a fieldless struct/component), and VE_REFLECT (a fielded struct).
    /// TypeRegistry::Register reads the trait; with the member set uniform, the
    /// two compile-time dispatchers below are direct reads.
    template <class T>
    struct VengReflect;

    /// @brief The stable TypeId of any reflected type, read as a compile-time constant off its trait.
    ///
    /// Independent of registration order and of any TypeRegistry instance.
    template <class T>
    constexpr TypeId TypeIdOf()
    {
        return VengReflect<T>::Id;
    }

    /// @brief The FieldClass of any reflected type, read as a compile-time constant off its trait.
    template <class T>
    constexpr FieldClass FieldClassOf()
    {
        return VengReflect<T>::Class;
    }
}

/// @brief Declares a non-struct leaf type's identity by specialising VengReflect\<T\>.
///
/// Emits the given TypeId + FieldClass, a Name() that yields the type spelling,
/// an empty Fields(), and a no-op RegisterDependencies. Game code adds a leaf
/// or enum the same way — `VE_LEAF(::MyGame::MyEnum, 0x…ULL, FieldClass::Enum)` —
/// with no engine change. The id is an authored 0x…ULL literal (engine builtins)
/// or a `vengc generate-id` value (game types).
///
/// The type **must be written fully qualified** (a leading `::`), so the registry
/// records its namespace; a fundamental type (`bool`, …), which has none, is the
/// sole exception and is spelled bare.
#define VE_LEAF(Type, TypeIdLiteral, FieldClassValue)                                              \
    template <>                                                                                    \
    struct ::Veng::VengReflect<Type>                                                               \
    {                                                                                              \
        static_assert(::Veng::Detail::IsFullyQualifiedSpelling(#Type) ||                           \
                          std::is_fundamental_v<Type>,                                             \
                      "VE_LEAF: the type must be written fully qualified, e.g. ::Veng::Foo");      \
        static constexpr ::Veng::TypeId Id = (TypeIdLiteral);                                      \
        static constexpr ::Veng::FieldClass Class = (FieldClassValue);                             \
        static ::Veng::string Name() { return #Type; }                                             \
        static ::Veng::vector<::Veng::FieldDescriptor> Fields() { return {}; }                     \
        static void RegisterDependencies(::Veng::TypeRegistry&) {}                                 \
    }

// ----- Builtin leaf vocabulary ---------------------------------------------
// Each carries a hardcoded TypeId literal, exactly like the core pack's built-in
// asset ids, plus its meta-kind. Distinct C++ types that share a representation
// (bool vs u8) still get distinct ids — identity is per C++ type, not per byte
// layout. Authored at global scope with fully-qualified tokens, so the registry
// records "Veng" as the namespace and the bare name (bool, a fundamental type,
// carries no namespace and stays unqualified).

/// @cond DOXYGEN_EXCLUDE
VE_LEAF(bool, 0x283EDB5B266A27EDULL, ::Veng::FieldClass::Scalar);
VE_LEAF(::Veng::f32, 0x4AF0D89664A476FBULL, ::Veng::FieldClass::Scalar);
VE_LEAF(::Veng::i32, 0xE4A543818EB46182ULL, ::Veng::FieldClass::Scalar);
VE_LEAF(::Veng::u32, 0x6AD25BC2BE1A5D65ULL, ::Veng::FieldClass::Scalar);
VE_LEAF(::Veng::u64, 0x94AB42FEF4E32D87ULL, ::Veng::FieldClass::Scalar);
VE_LEAF(::Veng::vec2, 0xB9A6A5F871901160ULL, ::Veng::FieldClass::Vector);
VE_LEAF(::Veng::vec3, 0xA9A78263CAA293E7ULL, ::Veng::FieldClass::Vector);
VE_LEAF(::Veng::vec4, 0xA936BFC80085F684ULL, ::Veng::FieldClass::Vector);
VE_LEAF(::Veng::uvec2, 0x2113083C89AF9C6CULL, ::Veng::FieldClass::Vector);
VE_LEAF(::Veng::quat, 0xFD92495C91720213ULL, ::Veng::FieldClass::Quaternion);
VE_LEAF(::Veng::mat4, 0x8ABB4818B9CC633EULL, ::Veng::FieldClass::Matrix);
VE_LEAF(::Veng::string, 0x2E46B7DE1FFC7DFCULL, ::Veng::FieldClass::String);
VE_LEAF(::Veng::AssetHandle<::Veng::Texture>, 0x612EE7E69BE7B848ULL,
        ::Veng::FieldClass::AssetHandle);
VE_LEAF(::Veng::AssetHandle<::Veng::Mesh>, 0x1CD2C85C50AFC9E0ULL, ::Veng::FieldClass::AssetHandle);
VE_LEAF(::Veng::AssetHandle<::Veng::Material>, 0x3992D11EB4362B4CULL,
        ::Veng::FieldClass::AssetHandle);
VE_LEAF(::Veng::AssetHandle<::Veng::MaterialInstance>, 0xB47397CC23B08FDEULL,
        ::Veng::FieldClass::AssetHandle);
VE_LEAF(::Veng::AssetHandle<::Veng::Prefab>, 0xF71230AEA9060D83ULL,
        ::Veng::FieldClass::AssetHandle);
VE_LEAF(::Veng::AssetHandle<::Veng::Animation>, 0xED6B03478BD050CEULL,
        ::Veng::FieldClass::AssetHandle);
VE_LEAF(::Veng::AssetHandle<::Veng::Environment>, 0x4E2499935571083DULL,
        ::Veng::FieldClass::AssetHandle);

// Entity is an intra-scene reference, not a value leaf — the prefab loader
// recognises FieldClass::Reference to remap it into a fresh Scene.
VE_LEAF(::Veng::Entity, 0x448BDF481E27075EULL, ::Veng::FieldClass::Reference);
/// @endcond
