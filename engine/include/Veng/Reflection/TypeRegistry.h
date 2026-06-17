#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/FieldDescriptor.h>

#include <new>
#include <utility>

namespace Veng
{
    // The recorded description of a registered type: name, layout, the
    // construct/destruct/move thunks a type-erased pool drives, the meta-kind,
    // and — for Struct-class types — the field descriptors a generic walk reads
    // to serialize a value without knowing its C++ type.
    struct TypeInfo
    {
        // Logs / editor display only — never the persisted key. The TypeId is
        // the identity on disk.
        string Name;
        usize Size = 0;
        usize Align = 0;
        void (*DefaultConstruct)(void* dst) = nullptr;
        void (*Destruct)(void* obj) = nullptr;
        // Used for swap-and-pop on remove: move the tail element into the hole.
        void (*MoveConstruct)(void* dst, void* src) = nullptr;
        TypeId Id = InvalidTypeId;
        FieldClass Class = FieldClass::Struct;
        vector<FieldDescriptor> Fields;
    };

    // Records each registered type under its authored TypeId, keyed by that id.
    // Registration is main-thread, startup-only and must complete before any
    // Scene that pools the types is touched concurrently with task-system
    // workers. The registry is engine-owned (Application::GetTypeRegistry()) and
    // threaded into Scene::Create — no global.
    class TypeRegistry
    {
    public:
        // Synthesises T's lifecycle thunks and records its TypeInfo under
        // VengReflect<T>::Id, lifecycle-only (no fields). Registering two
        // distinct types under the same id is a fatal collision assert. Returns
        // the recorded id.
        template <class T>
        TypeId Register(string name)
        {
            constexpr TypeId id = VengReflect<T>::Id;
            static_assert(id != InvalidTypeId,
                          "VengReflect<T>::Id must be a non-zero authored id");
            return RegisterImpl<T>(id, std::move(name), FieldClass::Struct, {});
        }

        // Explicit — for leaves, hand-authored types, anything a macro can't
        // express. Records T's lifecycle thunks plus the given fields under id.
        template <class T>
        TypeId Register(string name, FieldClass cls, vector<FieldDescriptor> fields)
        {
            constexpr TypeId id = TypeIdOf<T>();
            static_assert(id != InvalidTypeId,
                          "TypeIdOf<T>() must be a non-zero authored id");
            return RegisterImpl<T>(id, std::move(name), cls, std::move(fields));
        }

        // Trait-driven — reads VengReflect<T> (its Name, Class, and Fields). The
        // single registration path for every reflected type: a leaf's Fields()
        // is {} (so nothing extra is recorded) and its RegisterDependencies is a
        // no-op; a struct's replay its describe-block. Idempotent: re-registering
        // the same id is a no-op (returns the id), so auto-registration of a
        // referenced type can be called freely.
        template <class T>
        TypeId Register()
        {
            constexpr TypeId id = VengReflect<T>::Id;
            static_assert(id != InvalidTypeId,
                          "VengReflect<T>::Id must be a non-zero authored id");

            if (m_Types.contains(id))
                return id;

            const TypeId registered = RegisterImpl<T>(
                id, VengReflect<T>::Name(), VengReflect<T>::Class, VengReflect<T>::Fields());

            // Auto-register each field's type from its own trait, recursively and
            // idempotently, so referencing a nested type carries no
            // registration-ordering burden. A leaf field's Fields() is empty so
            // it bottoms out the recursion. Registered after T itself so a
            // self-referential type's id is already present (the contains() guard
            // above then short-circuits the recursion).
            VengReflect<T>::RegisterDependencies(*this);
            return registered;
        }

        // The authored TypeId of T, read straight off its trait — a compile-time
        // constant, independent of registration order or this instance.
        template <class T>
        [[nodiscard]] constexpr TypeId IdOf() const
        {
            return TypeIdOf<T>();
        }

        [[nodiscard]] const TypeInfo& Info(TypeId id) const
        {
            const auto it = m_Types.find(id);
            VE_ASSERT(it != m_Types.end(), "TypeId {:#018x} is not registered", id);
            return it->second;
        }

        [[nodiscard]] bool IsRegistered(TypeId id) const
        {
            return m_Types.contains(id);
        }

        [[nodiscard]] usize Count() const { return m_Types.size(); }

        // Read-only view over every registered (id, info) pair, for tooling that
        // enumerates the table (a reflected type manifest, an editor's type
        // picker). Iteration order is unspecified.
        [[nodiscard]] const unordered_map<TypeId, TypeInfo>& All() const { return m_Types; }

    private:
        template <class T>
        TypeId RegisterImpl(TypeId id, string name, FieldClass cls, vector<FieldDescriptor> fields)
        {
            const auto existing = m_Types.find(id);
            VE_ASSERT(existing == m_Types.end(),
                      "TypeId collision: '{}' and '{}' both claim TypeId {:#018x}",
                      name, existing == m_Types.end() ? string{} : existing->second.Name,
                      id);

            TypeInfo info;
            info.Name = std::move(name);
            info.Size = sizeof(T);
            info.Align = alignof(T);
            info.DefaultConstruct = [](void* dst) { ::new (dst) T{}; };
            info.Destruct = [](void* obj) { static_cast<T*>(obj)->~T(); };
            info.MoveConstruct = [](void* dst, void* src)
            {
                ::new (dst) T{std::move(*static_cast<T*>(src))};
            };
            info.Id = id;
            info.Class = cls;
            info.Fields = std::move(fields);

            m_Types.emplace(id, std::move(info));
            return id;
        }

        unordered_map<TypeId, TypeInfo> m_Types;
    };
}

// Declares a fieldless struct/component's identity by specialising
// VengReflect<T>: the given TypeId, Class = Struct, a Name() that yields the
// type spelling, an empty Fields(), and a no-op RegisterDependencies — so it
// flows through the same uniform Register<T>() as everything else. Use it for a
// poolable type that needs an id but carries no fields; a fielded struct uses
// VE_REFLECT and a non-struct leaf/enum uses VE_LEAF. The id is an authored
// 0x…ULL literal (engine builtins) or a `vengc generate-id` value (game types).
#define VE_TYPE(Type, TypeIdLiteral)                                            \
    template <>                                                                 \
    struct ::Veng::VengReflect<Type>                                           \
    {                                                                          \
        static constexpr ::Veng::TypeId Id = (TypeIdLiteral);                  \
        static constexpr ::Veng::FieldClass Class = ::Veng::FieldClass::Struct;\
        static ::Veng::string Name() { return #Type; }                         \
        static ::Veng::vector<::Veng::FieldDescriptor> Fields() { return {}; } \
        static void RegisterDependencies(::Veng::TypeRegistry&) {}             \
    }
