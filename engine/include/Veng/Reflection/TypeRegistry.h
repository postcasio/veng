#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Reflection/FieldDisplay.h>

#include <new>
#include <string_view>
#include <utility>

namespace Veng
{
    namespace Detail
    {
        /// @brief Detects whether VengReflect\<T\> exposes the VE_ENUM Enumerators() accessor.
        ///
        /// True only for an enum authored with VE_ENUM (which adds the accessor); a bare
        /// VE_LEAF(…, Enum) leaves it absent, so its TypeInfo::Enumerators stays empty.
        template <class T>
        concept HasEnumerators = requires { VengReflect<T>::Enumerators(); };
    }

    /// @brief The recorded description of a registered type.
    ///
    /// Carries the name, layout, construct/destruct/move thunks a type-erased
    /// pool drives, the meta-kind, and — for Struct-class types — the field
    /// descriptors a generic walk reads to serialize a value without knowing its
    /// C++ type.
    struct TypeInfo
    {
        /// @brief The bare type name (qualifiers stripped) — logs/editor display only, never the persisted key.
        ///
        /// The TypeId is the on-disk identity; this is split from the authored spelling by
        /// SplitQualifiedTypeName, with the enclosing namespace held in Namespace.
        string Name;
        /// @brief The enclosing namespace of the type (e.g. "Veng"); empty for a global-namespace type.
        string Namespace;
        /// @brief The fully-qualified name "Namespace::Name" (just "Name" when global) — the matching key.
        ///
        /// The single spelling all type-name matching (cook-time JSON keys, variant tags)
        /// is done against; held as a field so a consumer need not reassemble it.
        string QualifiedName;
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
        /// @brief The type's default presentation, authored via VE_DISPLAY; the type-default arm of the cascade.
        FieldDisplay Display;
        /// @brief Enum-only: the {name, value} table in declaration order; empty for non-enums.
        ///
        /// Filled from VengReflect<T>::Enumerators() for a Class == Enum type authored with
        /// VE_ENUM; the editor draws a named combo from it and matches a backing value to its
        /// enumerator. A bare VE_LEAF(…, Enum) leaves it empty (the editor's integer fallback).
        vector<EnumEntry> Enumerators;

        /// @brief Variant-only: the alternative TypeIds, in declaration order.
        ///
        /// Empty for non-variant types.
        vector<TypeId> VariantAlternatives;
        /// @brief Variant-only: returns the active alternative's TypeId (InvalidTypeId = empty).
        ///
        /// Null for non-variant types.
        TypeId (*VariantActiveType)(const void*) = nullptr;
        /// @brief Variant-only: returns the active member's storage, or nullptr when empty.
        ///
        /// Null for non-variant types.
        void* (*VariantActivePtr)(void*) = nullptr;
        /// @brief Variant-only: returns the active member's const storage, or nullptr when empty.
        ///
        /// Null for non-variant types.
        const void* (*VariantActivePtrConst)(const void*) = nullptr;
        /// @brief Variant-only: activates `id`'s alternative (default-constructed); nullptr if `id` is not an alternative.
        ///
        /// Null for non-variant types.
        void* (*VariantSetActive)(void*, TypeId) = nullptr;
        /// @brief Variant-only: resets the variant to empty, destructing any active alternative.
        ///
        /// Null for non-variant types.
        void (*VariantClear)(void*) = nullptr;
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
            {
                return id;
            }

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

            QualifiedTypeName qualified = SplitQualifiedTypeName(name);

            TypeInfo info;
            info.QualifiedName = qualified.Namespace.empty()
                                     ? qualified.Name
                                     : qualified.Namespace + "::" + qualified.Name;
            info.Name = std::move(qualified.Name);
            info.Namespace = std::move(qualified.Namespace);
            info.Size = sizeof(T);
            info.Align = alignof(T);
            info.DefaultConstruct = [](void* dst) { ::new (dst) T{}; };
            info.Destruct = [](void* obj) { static_cast<T*>(obj)->~T(); };
            info.MoveConstruct = [](void* dst, void* src)
            { ::new (dst) T{std::move(*static_cast<T*>(src))}; };
            info.Id = id;
            info.Class = cls;
            info.Fields = std::move(fields);
            info.Display = VengDisplay<T>::Get();

            // An enum authored with VE_ENUM carries its {name, value} table; record it for
            // the editor's named combo. A bare VE_LEAF(…, Enum) has no accessor and stays empty.
            if constexpr (Detail::HasEnumerators<T>)
            {
                info.Enumerators = VengReflect<T>::Enumerators();
            }

            // A variant's active member is reached through type-erased thunks, never by
            // offset; record them off the VE_VARIANT specialisation for the generic walk.
            if constexpr (VengReflect<T>::Class == FieldClass::Variant)
            {
                info.VariantActiveType = &VengReflect<T>::ActiveType;
                info.VariantActivePtr = &VengReflect<T>::ActivePtr;
                info.VariantActivePtrConst = &VengReflect<T>::ActivePtrConst;
                info.VariantSetActive = &VengReflect<T>::SetActive;
                info.VariantClear = &VengReflect<T>::Clear;
                info.VariantAlternatives = VengReflect<T>::Alternatives();
            }

            m_Types.emplace(id, std::move(info));
            return id;
        }

        /// @brief All registered types, keyed by their authored TypeId.
        unordered_map<TypeId, TypeInfo> m_Types;
    };

    /// @brief True when `key` is the fully-qualified name of `info`, ignoring a leading "::".
    ///
    /// The cook-time matcher for a JSON component key or a variant `"type"` tag. Matching is
    /// strict: only the fully-qualified `QualifiedName` matches — a bare unqualified name does
    /// not. A leading "::" (the global-scope marker) on `key` is tolerated, since it denotes
    /// the same name.
    /// @param info  The registered type to test against.
    /// @param key   The authored spelling.
    /// @return True when `key` is the type's fully-qualified name.
    inline bool TypeNameMatches(const TypeInfo& info, std::string_view key)
    {
        if (key.size() >= 2 && key[0] == ':' && key[1] == ':')
        {
            key.remove_prefix(2);
        }
        return key == info.QualifiedName;
    }
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
        static_assert(::Veng::Detail::IsFullyQualifiedSpelling(#Type),                             \
                      "VE_TYPE: the type must be written fully qualified, e.g. ::Veng::Foo");      \
        static constexpr ::Veng::TypeId Id = (TypeIdLiteral);                                      \
        static constexpr ::Veng::FieldClass Class = ::Veng::FieldClass::Struct;                    \
        static ::Veng::string Name() { return #Type; }                                             \
        static ::Veng::vector<::Veng::FieldDescriptor> Fields() { return {}; }                     \
        static void RegisterDependencies(::Veng::TypeRegistry&) {}                                 \
    }
