#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Reflection/TypeRegistry.h>

#include <cstddef>

// VE_REFLECT — the reflection authoring surface for fielded structs. Placed next
// to a struct, it specialises VengReflect<T> with the type's stable TypeId plus
// its field descriptors, so the zero-arg TypeRegistry::Register<T>() reads the
// schema back at startup. The struct stays the single source of truth; only the
// field names are restated, once. The type is named **fully qualified** (a leading
// `::`), a hard rule a static_assert enforces, so the registry captures its namespace.
//
//   namespace Game { struct Transform { vec3 Position; quat Rotation; vec3 Scale; }; }
//
//   VE_REFLECT(::Game::Transform, 0x4DD9F2A1C03B5E76ULL)
//       VE_FIELD(Position, .DisplayName = "Position")
//       VE_FIELD(Rotation, .DisplayName = "Rotation")
//       VE_FIELD(Scale,    .DisplayName = "Scale", .Display = {.Min = 0.001})
//   VE_REFLECT_END();
//
// VE_FIELD(member, …) derives the field's Type and Class from decltype(T::member)
// at compile time; the trailing … are optional designated-initialiser editor
// metadata. A struct-class field's nested type is auto-registered on reference,
// so there is no registration-ordering burden.

namespace Veng::Detail
{
    /// @brief Sets DisplayName to Name when the author omits it.
    inline FieldDescriptor FinishField(FieldDescriptor desc)
    {
        if (desc.DisplayName.empty())
        {
            desc.DisplayName = desc.Name;
        }
        return desc;
    }

    /// @brief Fills the array shims of `desc` for a `vector<Element>` field.
    ///
    /// The four shims are type-erased over `Array = vector<Element>`: they take the
    /// field pointer (the vector storage) and address elements by index, so the
    /// generic serializer walks an array without naming Element. Sets ElementType to
    /// the element's authored TypeId so the read side resolves its schema.
    /// @tparam Element  The array's element type.
    /// @param desc  The descriptor to populate; Class must already be Array.
    /// @return The descriptor with its array shims and ElementType set.
    template <class Element>
    FieldDescriptor FinishArrayField(FieldDescriptor desc)
    {
        using Array = vector<Element>;
        desc.ElementType = TypeIdOf<Element>();
        desc.ArraySize = [](const void* arrayPtr)
        { return static_cast<const Array*>(arrayPtr)->size(); };
        desc.ArrayElement = [](void* arrayPtr, usize index) -> void*
        { return &(*static_cast<Array*>(arrayPtr))[index]; };
        desc.ArrayElementConst = [](const void* arrayPtr, usize index) -> const void*
        { return &(*static_cast<const Array*>(arrayPtr))[index]; };
        desc.ArrayResize = [](void* arrayPtr, usize count)
        { static_cast<Array*>(arrayPtr)->resize(count); };
        return desc;
    }

    /// @brief Sink that the describe-block drives once to collect a type's fields.
    struct FieldCollector
    {
        /// @brief Accumulated field descriptors.
        vector<FieldDescriptor> Fields;

        /// @brief Appends one finished field descriptor to the collected list.
        /// @tparam Owner  The struct type declaring the field.
        /// @tparam Field  The field's value type.
        template <class Owner, class Field>
        void Field_(FieldDescriptor desc)
        {
            Fields.push_back(FinishField(std::move(desc)));
        }

        /// @brief Appends one finished array-field descriptor, populating its element shims.
        /// @tparam Owner    The struct type declaring the field.
        /// @tparam Element  The array's element type.
        template <class Owner, class Element>
        void ArrayField_(FieldDescriptor desc)
        {
            Fields.push_back(FinishField(FinishArrayField<Element>(std::move(desc))));
        }
    };

    /// @brief Sink that the same describe-block drives to auto-register each field's type.
    ///
    /// A struct-class field recurses (carrying its own fields); a leaf field records
    /// itself with an empty field list and bottoms out the recursion. Idempotent.
    struct DependencyRegistrar
    {
        /// @brief The registry receiving each field's type registration.
        TypeRegistry& Registry;

        /// @brief Registers the field's type, recursing through struct-class fields.
        /// @tparam Owner  The struct type declaring the field.
        /// @tparam Field  The field's value type, registered into the registry.
        template <class Owner, class Field>
        void Field_(const FieldDescriptor&)
        {
            Registry.Register<Field>();
        }

        /// @brief Registers an array field's *element* type — the container has no trait.
        /// @tparam Owner    The struct type declaring the field.
        /// @tparam Element  The array's element type, registered into the registry.
        template <class Owner, class Element>
        void ArrayField_(const FieldDescriptor&)
        {
            Registry.Register<Element>();
        }
    };
}

// ---- The describe-block ---------------------------------------------------
//
// The block body is emitted once into a templated Describe(Sink&) member; both
// Fields() and RegisterDependencies() replay it with a different sink, so the
// field names are written exactly once.

/// @brief Opens a VE_ENUM block: declares an enum leaf and starts its enumerator list.
///
/// Mirrors VE_LEAF's identity emission (Id / Class = Enum / Name / empty Fields /
/// no-op RegisterDependencies) and adds an Enumerators() accessor that the registry
/// reads into TypeInfo::Enumerators. Each VE_ENUMERATOR records the enum constant's
/// own value cast to i64, so explicit or gapped values reflect correctly. The type
/// must be written fully qualified (a leading `::`).
#define VE_ENUM(Type, TypeIdLiteral)                                                               \
    template <>                                                                                    \
    struct ::Veng::VengReflect<Type>                                                               \
    {                                                                                              \
        static_assert(::Veng::Detail::IsFullyQualifiedSpelling(#Type),                             \
                      "VE_ENUM: the type must be written fully qualified, e.g. ::Veng::Foo");      \
        using Owner = Type;                                                                        \
        static constexpr ::Veng::TypeId Id = (TypeIdLiteral);                                      \
        static constexpr ::Veng::FieldClass Class = ::Veng::FieldClass::Enum;                      \
        static ::Veng::string Name() { return #Type; }                                             \
        static ::Veng::vector<::Veng::FieldDescriptor> Fields() { return {}; }                     \
        static void RegisterDependencies(::Veng::TypeRegistry&) {}                                 \
        static ::Veng::vector<::Veng::EnumEntry> Enumerators()                                     \
        {                                                                                          \
            ::Veng::vector<::Veng::EnumEntry> entries;

/// @brief Records one enumerator within a VE_ENUM block: its name and the enum constant's value.
#define VE_ENUMERATOR(Enumerator)                                                                  \
    entries.push_back(::Veng::EnumEntry{.Name = #Enumerator,                                       \
                                        .Value = static_cast<::Veng::i64>(Owner::Enumerator)});

/// @brief Closes a VE_ENUM block and emits the assembled Enumerators().
#define VE_ENUM_END()                                                                              \
    return entries;                                                                                \
    }                                                                                              \
    }

/// @brief Opens VengReflect\<T\>: names the owning type and starts the shared Describe body.
///
/// Both public entry points (Fields / RegisterDependencies) call into it.
#define VE_REFLECT(Type, TypeIdLiteral)                                                            \
    template <>                                                                                    \
    struct ::Veng::VengReflect<Type>                                                               \
    {                                                                                              \
        static_assert(::Veng::Detail::IsFullyQualifiedSpelling(#Type),                             \
                      "VE_REFLECT: the type must be written fully qualified, e.g. ::Veng::Foo");   \
        using Owner = Type;                                                                        \
        static constexpr ::Veng::TypeId Id = (TypeIdLiteral);                                      \
        static ::Veng::string Name() { return #Type; }                                             \
        static constexpr ::Veng::FieldClass Class = ::Veng::FieldClass::Struct;                    \
        template <class Sink>                                                                      \
        static void Describe([[maybe_unused]] Sink& sink)                                          \
        {

/// @brief Declares one field record within a VE_REFLECT block.
///
/// Type and Class are compile-time constants read off the field type's trait;
/// the trailing … are optional designated-initialiser editor metadata.
#define VE_FIELD(Member, ...)                                                                      \
    sink.template Field_<Owner, decltype(Owner::Member)>(                                          \
        ::Veng::FieldDescriptor{.Name = #Member,                                                   \
                                .Type = ::Veng::TypeIdOf<decltype(Owner::Member)>(),               \
                                .Class = ::Veng::FieldClassOf<decltype(Owner::Member)>(),          \
                                .Offset = offsetof(Owner, Member),                                 \
                                __VA_ARGS__});

/// @brief Declares one array-field record (a `vector<T>` member) within a VE_REFLECT block.
///
/// The element type is read off the member's `value_type` at compile time; the
/// descriptor carries Class = Array and the element-access shims, and the element
/// type is auto-registered like any other field type. The container itself carries
/// no trait, so Type stays InvalidTypeId — the element schema lives in ElementType.
/// The trailing … are optional designated-initialiser editor metadata.
#define VE_ARRAY_FIELD(Member, ...)                                                                \
    sink.template ArrayField_<Owner, typename decltype(Owner::Member)::value_type>(                \
        ::Veng::FieldDescriptor{.Name = #Member,                                                   \
                                .Class = ::Veng::FieldClass::Array,                                \
                                .Offset = offsetof(Owner, Member),                                 \
                                __VA_ARGS__});

/// @brief Closes a VE_REFLECT block and emits Fields() / RegisterDependencies().
#define VE_REFLECT_END()                                                                           \
    }                                                                                              \
    static ::Veng::vector<::Veng::FieldDescriptor> Fields()                                        \
    {                                                                                              \
        ::Veng::Detail::FieldCollector collector;                                                  \
        Describe(collector);                                                                       \
        return std::move(collector.Fields);                                                        \
    }                                                                                              \
    static void RegisterDependencies(::Veng::TypeRegistry& registry)                               \
    {                                                                                              \
        ::Veng::Detail::DependencyRegistrar registrar{registry};                                   \
        Describe(registrar);                                                                       \
    }                                                                                              \
    }

/// @brief Declares a Variant\<Ts...\>'s identity by specialising VengReflect\<T\>.
///
/// Type already is a Variant\<Ts...\> (or an alias of one), so the alternatives are
/// reachable through the type's own static members — the macro never restates the pack.
/// It emits Class = Variant, an empty Fields(), the type-erased variant thunks the
/// registry records on the TypeInfo, and a RegisterDependencies that registers each
/// alternative. The id is an authored 0x…ULL literal (engine builtins) or a
/// `vengc generate-type-id` value (game types).
#define VE_VARIANT(Type, TypeIdLiteral)                                                            \
    template <>                                                                                    \
    struct ::Veng::VengReflect<Type>                                                               \
    {                                                                                              \
        static_assert(::Veng::Detail::IsFullyQualifiedSpelling(#Type),                             \
                      "VE_VARIANT: the type must be written fully qualified, e.g. ::Veng::Foo");   \
        static constexpr ::Veng::TypeId Id = (TypeIdLiteral);                                      \
        static constexpr ::Veng::FieldClass Class = ::Veng::FieldClass::Variant;                   \
        static ::Veng::string Name() { return #Type; }                                             \
        static ::Veng::vector<::Veng::FieldDescriptor> Fields() { return {}; }                     \
        static ::Veng::TypeId ActiveType(const void* p)                                            \
        {                                                                                          \
            return static_cast<const Type*>(p)->ActiveType();                                      \
        }                                                                                          \
        static void* ActivePtr(void* p) { return static_cast<Type*>(p)->ActivePtr(); }             \
        static const void* ActivePtrConst(const void* p)                                           \
        {                                                                                          \
            return static_cast<const Type*>(p)->ActivePtr();                                       \
        }                                                                                          \
        static void* SetActive(void* p, ::Veng::TypeId id)                                         \
        {                                                                                          \
            return static_cast<Type*>(p)->SetActive(id);                                           \
        }                                                                                          \
        static void Clear(void* p) { static_cast<Type*>(p)->Clear(); }                             \
        static ::Veng::vector<::Veng::TypeId> Alternatives()                                       \
        {                                                                                          \
            const auto s = Type::Alternatives();                                                   \
            return {s.begin(), s.end()};                                                           \
        }                                                                                          \
        static void RegisterDependencies(::Veng::TypeRegistry& r)                                  \
        {                                                                                          \
            Type::RegisterAlternatives(r);                                                         \
        }                                                                                          \
    }
