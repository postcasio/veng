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
/// or enum the same way — `VE_LEAF(MyEnum, 0x…ULL, FieldClass::Enum)` — with
/// no engine change. The id is an authored 0x…ULL literal (engine builtins) or
/// a `vengc generate-id` value (game types).
#define VE_LEAF(Type, TypeIdLiteral, FieldClassValue)                          \
    template <>                                                                 \
    struct ::Veng::VengReflect<Type>                                           \
    {                                                                          \
        static constexpr ::Veng::TypeId Id = (TypeIdLiteral);                  \
        static constexpr ::Veng::FieldClass Class = (FieldClassValue);         \
        static ::Veng::string Name() { return #Type; }                         \
        static ::Veng::vector<::Veng::FieldDescriptor> Fields() { return {}; } \
        static void RegisterDependencies(::Veng::TypeRegistry&) {}             \
    }

// ----- Builtin leaf vocabulary ---------------------------------------------
// Each carries a hardcoded TypeId literal, exactly like the core pack's built-in
// asset ids, plus its meta-kind. Distinct C++ types that share a representation
// (bool vs u8) still get distinct ids — identity is per C++ type, not per byte
// layout. Authored inside namespace Veng so the unqualified type spelling is both
// the template argument and the stringised TypeInfo.Name; the macro's own
// ::Veng:: qualifier on the trait it specialises is then redundant here, hence
// the local diagnostic suppression.

#if defined(__clang__) || defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wextra-qualification"
#endif

namespace Veng
{
    VE_LEAF(bool, 0x283EDB5B266A27EDULL, FieldClass::Scalar);
    VE_LEAF(f32, 0x4AF0D89664A476FBULL, FieldClass::Scalar);
    VE_LEAF(i32, 0xE4A543818EB46182ULL, FieldClass::Scalar);
    VE_LEAF(u32, 0x6AD25BC2BE1A5D65ULL, FieldClass::Scalar);
    VE_LEAF(u64, 0x94AB42FEF4E32D87ULL, FieldClass::Scalar);
    VE_LEAF(vec2, 0xB9A6A5F871901160ULL, FieldClass::Vector);
    VE_LEAF(vec3, 0xA9A78263CAA293E7ULL, FieldClass::Vector);
    VE_LEAF(vec4, 0xA936BFC80085F684ULL, FieldClass::Vector);
    VE_LEAF(quat, 0xFD92495C91720213ULL, FieldClass::Quaternion);
    VE_LEAF(mat4, 0x8ABB4818B9CC633EULL, FieldClass::Matrix);
    VE_LEAF(string, 0x2E46B7DE1FFC7DFCULL, FieldClass::String);
    VE_LEAF(AssetHandle<Texture>, 0x612EE7E69BE7B848ULL, FieldClass::AssetHandle);
    VE_LEAF(AssetHandle<Mesh>, 0x1CD2C85C50AFC9E0ULL, FieldClass::AssetHandle);
    VE_LEAF(AssetHandle<Material>, 0x3992D11EB4362B4CULL, FieldClass::AssetHandle);

    // Entity is an intra-scene reference, not a value leaf — the prefab loader
    // recognises FieldClass::Reference to remap it into a fresh Scene.
    VE_LEAF(Entity, 0x448BDF481E27075EULL, FieldClass::Reference);
}

#if defined(__clang__) || defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif
