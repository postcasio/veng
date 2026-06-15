#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Reflection/TypeRegistry.h>

#include <cstddef>

// VE_REFLECT — the v1 reflection authoring surface. Placed at namespace scope
// next to a struct, it specialises VengReflect<T> with the type's stable TypeId
// plus its field descriptors, so the zero-arg TypeRegistry::Register<T>() reads
// the schema back at startup. The struct stays the single source of truth; only
// the field names are restated, once.
//
//   struct Transform { vec3 Position; quat Rotation; vec3 Scale; };
//
//   VE_REFLECT(Transform, 0x4DD9F2A1C03B5E76ULL)
//       VE_FIELD(Position, .DisplayName = "Position")
//       VE_FIELD(Rotation, .DisplayName = "Rotation")
//       VE_FIELD(Scale,    .DisplayName = "Scale", .Min = 0.001)
//   VE_REFLECT_END();
//
// VE_FIELD(member, …) derives the field's Type and Class from decltype(T::member)
// at compile time; the trailing … are optional designated-initialiser editor
// metadata. A struct-class field's nested type is auto-registered on reference,
// so there is no registration-ordering burden.

namespace Veng::Detail
{
    // DisplayName falls back to the serialization key when the author omits it.
    inline FieldDescriptor FinishField(FieldDescriptor desc)
    {
        if (desc.DisplayName.empty())
            desc.DisplayName = desc.Name;
        return desc;
    }

    // Sink that the describe-block drives once to collect a type's fields.
    struct FieldCollector
    {
        vector<FieldDescriptor> Fields;

        template <class Owner, class Field>
        void Field_(FieldDescriptor desc)
        {
            Fields.push_back(FinishField(std::move(desc)));
        }
    };

    // Sink that the same describe-block drives to auto-register each field's
    // type: a struct-class field recurses through Register<T>() (carrying its own
    // fields); a leaf field is recorded through EnsureLeaf<T>() so a generic walk
    // can read its Size off its TypeInfo. Both are idempotent.
    struct DependencyRegistrar
    {
        TypeRegistry& Registry;

        template <class Owner, class Field>
        void Field_(const FieldDescriptor&)
        {
            if constexpr (FieldClassOf<Field>() == FieldClass::Struct)
                Registry.Register<Field>();
            else
                Registry.EnsureLeaf<Field>();
        }
    };
}

// ---- The describe-block ---------------------------------------------------
//
// The block body is emitted once into a templated Describe(Sink&) member; both
// Fields() and RegisterDependencies() replay it with a different sink, so the
// field names are written exactly once.

// Opens VengReflect<T>: names the owning type and starts the shared Describe
// body. The two public entry points (Fields / RegisterDependencies) call into it.
#define VE_REFLECT(Type, TypeIdLiteral)                                         \
    template <>                                                                 \
    struct ::Veng::VengReflect<Type>                                            \
    {                                                                           \
        using Owner = Type;                                                     \
        static constexpr ::Veng::TypeId Id = (TypeIdLiteral);                   \
        static ::Veng::string Name() { return #Type; }                          \
        static constexpr ::Veng::FieldClass Class()                             \
        {                                                                       \
            return ::Veng::FieldClass::Struct;                                  \
        }                                                                       \
        template <class Sink>                                                   \
        static void Describe([[maybe_unused]] Sink& sink)                       \
        {

// One field record. Type and Class are compile-time constants read off the field
// type's trait; the trailing … are optional designated-initialiser editor
// metadata, applied last so they override the defaults above.
#define VE_FIELD(Member, ...)                                                   \
            sink.template Field_<Owner, decltype(Owner::Member)>(               \
                ::Veng::FieldDescriptor{                                        \
                    .Name = #Member,                                            \
                    .Type = ::Veng::TypeIdOf<decltype(Owner::Member)>(),       \
                    .Class = ::Veng::FieldClassOf<decltype(Owner::Member)>(),   \
                    .Offset = offsetof(Owner, Member),                         \
                    __VA_ARGS__ });

#define VE_REFLECT_END()                                                        \
        }                                                                       \
        static ::Veng::vector<::Veng::FieldDescriptor> Fields()                 \
        {                                                                       \
            ::Veng::Detail::FieldCollector collector;                           \
            Describe(collector);                                                \
            return std::move(collector.Fields);                                 \
        }                                                                       \
        static void RegisterDependencies(::Veng::TypeRegistry& registry)        \
        {                                                                       \
            ::Veng::Detail::DependencyRegistrar registrar{registry};            \
            Describe(registrar);                                                \
        }                                                                       \
    }
