#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/FieldDescriptor.h>

#include <new>
#include <utility>

namespace Veng
{
    /// @brief The recorded description of a registered type.
    ///
    /// Carries the name, layout, construct/destruct/move thunks a type-erased
    /// pool drives, the meta-kind, and — for Struct-class types — the field
    /// descriptors a generic walk reads to serialize a value without knowing its
    /// C++ type.
    struct TypeInfo
    {
        /// @brief Logs/editor display only — never the persisted key. The TypeId is the on-disk identity.
        string Name;
        /// @brief `sizeof(T)`.
        usize Size = 0;
        /// @brief `alignof(T)`.
        usize Align = 0;
        /// @brief Placement-new default-constructs T into dst.
        void (*DefaultConstruct)(void* dst) = nullptr;
        /// @brief Calls T's destructor in place.
        void (*Destruct)(void* obj) = nullptr;
        /// @brief Move-constructs T from src into dst; used for swap-and-pop on pool remove.
        void (*MoveConstruct)(void* dst, void* src) = nullptr;
        /// @brief The authored stable type identity.
        TypeId Id = InvalidTypeId;
        /// @brief The meta-kind — Struct for components, others for leaves.
        FieldClass Class = FieldClass::Struct;
        /// @brief Field descriptors for Struct-class types; empty for leaves.
        vector<FieldDescriptor> Fields;
    };

    /// @brief Maps authored TypeIds to their TypeInfo records.
    ///
    /// Registration is main-thread and startup-only; it must complete before any
    /// Scene that pools the types is used. The registry is owned by the host
    /// (launcher or cooker) and threaded into Scene::Create — no global.
    class TypeRegistry
    {
    public:
        /// @brief Registers T under VengReflect\<T\>::Id with lifecycle thunks only (no fields).
        ///
        /// Registering two distinct types under the same id is a fatal collision assert.
        /// @return The recorded TypeId.
        template <class T>
        TypeId Register(string name)
        {
            constexpr TypeId id = VengReflect<T>::Id;
            static_assert(id != InvalidTypeId, "VengReflect<T>::Id must be a non-zero authored id");
            return RegisterImpl<T>(id, std::move(name), FieldClass::Struct, {});
        }

        /// @brief Registers T with explicit FieldClass and field descriptors.
        ///
        /// For leaves, hand-authored types, or anything a macro cannot express.
        /// @return The recorded TypeId.
        template <class T>
        TypeId Register(string name, FieldClass cls, vector<FieldDescriptor> fields)
        {
            constexpr TypeId id = TypeIdOf<T>();
            static_assert(id != InvalidTypeId, "TypeIdOf<T>() must be a non-zero authored id");
            return RegisterImpl<T>(id, std::move(name), cls, std::move(fields));
        }

        /// @brief Trait-driven registration: reads VengReflect\<T\> for Name, Class, and Fields.
        ///
        /// The single registration path for every reflected type: a leaf's Fields()
        /// is `{}` and its RegisterDependencies is a no-op; a struct's replays its
        /// describe-block. Idempotent: re-registering the same id is a no-op.
        /// @return The recorded TypeId.
        template <class T>
        TypeId Register()
        {
            constexpr TypeId id = VengReflect<T>::Id;
            static_assert(id != InvalidTypeId, "VengReflect<T>::Id must be a non-zero authored id");

            if (m_Types.contains(id))
                return id;

            const TypeId registered = RegisterImpl<T>(
                id, VengReflect<T>::Name(), VengReflect<T>::Class, VengReflect<T>::Fields());

            // Auto-register each field's type so referencing a nested type carries
            // no registration-ordering burden. Runs after T's own entry is inserted
            // so a self-referential type's id is already present (the contains()
            // guard above then short-circuits the recursion).
            VengReflect<T>::RegisterDependencies(*this);
            return registered;
        }

        /// @brief The authored TypeId of T, read as a compile-time constant off its trait.
        ///
        /// Independent of registration order and of this registry instance.
        template <class T>
        [[nodiscard]] constexpr TypeId IdOf() const
        {
            return TypeIdOf<T>();
        }

        /// @brief Returns the TypeInfo for the given id; fatal assert if not registered.
        [[nodiscard]] const TypeInfo& Info(TypeId id) const
        {
            const auto it = m_Types.find(id);
            VE_ASSERT(it != m_Types.end(), "TypeId {:#018x} is not registered", id);
            return it->second;
        }

        /// @brief Returns true if the given TypeId has been registered.
        [[nodiscard]] bool IsRegistered(TypeId id) const { return m_Types.contains(id); }

        /// @brief Returns the number of registered types.
        [[nodiscard]] usize Count() const { return m_Types.size(); }

        /// @brief Read-only view over every registered (id, info) pair.
        ///
        /// For tooling that enumerates the table (a reflected type manifest, an
        /// editor type picker). Iteration order is unspecified.
        [[nodiscard]] const unordered_map<TypeId, TypeInfo>& All() const { return m_Types; }

    private:
        /// @brief Synthesises T's lifecycle thunks and inserts a TypeInfo under id; asserts on collision.
        template <class T>
        TypeId RegisterImpl(TypeId id, string name, FieldClass cls, vector<FieldDescriptor> fields)
        {
            const auto existing = m_Types.find(id);
            VE_ASSERT(existing == m_Types.end(),
                      "TypeId collision: '{}' and '{}' both claim TypeId {:#018x}", name,
                      existing == m_Types.end() ? string{} : existing->second.Name, id);

            TypeInfo info;
            info.Name = std::move(name);
            info.Size = sizeof(T);
            info.Align = alignof(T);
            info.DefaultConstruct = [](void* dst) { ::new (dst) T{}; };
            info.Destruct = [](void* obj) { static_cast<T*>(obj)->~T(); };
            info.MoveConstruct = [](void* dst, void* src)
            { ::new (dst) T{std::move(*static_cast<T*>(src))}; };
            info.Id = id;
            info.Class = cls;
            info.Fields = std::move(fields);

            m_Types.emplace(id, std::move(info));
            return id;
        }

        /// @brief All registered types, keyed by their authored TypeId.
        unordered_map<TypeId, TypeInfo> m_Types;
    };
}

/// @brief Declares a fieldless struct/component's identity by specialising VengReflect\<T\>.
///
/// Emits the given TypeId with Class = Struct, a Name() that yields the type
/// spelling, an empty Fields(), and a no-op RegisterDependencies — so it flows
/// through the same uniform Register\<T\>() as everything else. Use for a poolable
/// type that needs an id but carries no fields; a fielded struct uses VE_REFLECT
/// and a non-struct leaf/enum uses VE_LEAF. The id is an authored 0x…ULL literal
/// (engine builtins) or a `vengc generate-id` value (game types).
#define VE_TYPE(Type, TypeIdLiteral)                                                               \
    template <>                                                                                    \
    struct ::Veng::VengReflect<Type>                                                               \
    {                                                                                              \
        static constexpr ::Veng::TypeId Id = (TypeIdLiteral);                                      \
        static constexpr ::Veng::FieldClass Class = ::Veng::FieldClass::Struct;                    \
        static ::Veng::string Name() { return #Type; }                                             \
        static ::Veng::vector<::Veng::FieldDescriptor> Fields() { return {}; }                     \
        static void RegisterDependencies(::Veng::TypeRegistry&) {}                                 \
    }
