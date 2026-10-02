#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Reflection/FieldDisplay.h>

#include <atomic>
#include <new>
#include <string_view>
#include <utility>

namespace Veng
{
    /// @brief The ordinal no registered type carries: what TypeRegistry::OrdinalOf reports for an unregistered type.
    inline constexpr u32 InvalidTypeOrdinal = ~0u;

    namespace Detail
    {
        /// @brief Per-type cache of T's ordinal in the registry that last resolved it.
        ///
        /// Packs that registry's serial in the high 32 bits and the ordinal in the low 32, so one
        /// relaxed load answers both which registry the entry is for and what the ordinal is. No
        /// registry has serial zero, so an unfilled cache always misses. It is a cache, not state:
        /// each module image may hold its own copy, and every copy resolves to the same value.
        /// @tparam T  The type whose ordinal is cached.
        template <class T>
        inline std::atomic<u64> g_TypeOrdinalCache{0};

        /// @brief Detects whether VengReflect\<T\> exposes the VE_ENUM Enumerators() accessor.
        ///
        /// True only for an enum authored with VE_ENUM (which adds the accessor); a bare
        /// VE_LEAF(…, Enum) leaves it absent, so its TypeInfo::Enumerators stays empty.
        template <class T>
        concept HasEnumerators = requires { VengReflect<T>::Enumerators(); };

        /// @brief Collects the TypeIds of a component's required siblings, in declaration order.
        ///
        /// The pack VE_REQUIRES expands its type list into; naming the vector inside a template is
        /// what keeps it instantiated only where the specialisation's Required() is called.
        /// @tparam Ts  The required sibling component types.
        /// @return Their TypeIds, in the order written.
        template <class... Ts>
        vector<TypeId> RequiredIds()
        {
            return {TypeIdOf<Ts>()...};
        }
    }

    /// @brief Primary template authoring whether a type replicates over the wire; false unless VE_REPLICATED marks it.
    ///
    /// TypeRegistry::Register<T>() reads VengReplication<T>::Replicated into TypeInfo::Replicated.
    /// A separate specialisation point from VengReflect<T> (like VengDisplay<T>), so the mark
    /// composes with every reflection macro without touching them: a type opts a whole struct into
    /// snapshot encoding with a VE_REPLICATED tag beside its describe block.
    /// @tparam T  The type whose replication default is authored.
    template <class T>
    struct VengReplication
    {
        /// @brief Whether the type replicates; false for the primary template.
        static constexpr bool Replicated = false;
    };

    /// @brief Primary template authoring whether a replicated type is server-owned; false unless VE_SERVER_OWNED marks it.
    ///
    /// TypeRegistry::Register<T>() reads VengServerOwned<T>::ServerOwned into TypeInfo::ServerOwned.
    /// A server-owned type replicates like any other but is authoritative on every peer: a peer
    /// predicting an entity that carries it applies the snapshot's value directly and never compares,
    /// records, restores or replays it. A separate specialisation point from VengReflect<T>, like
    /// VengReplication<T>, so it composes with every reflection macro. Only meaningful on a
    /// replicated type, which registration asserts.
    /// @tparam T  The type whose server-owned default is authored.
    template <class T>
    struct VengServerOwned
    {
        /// @brief Whether the type is server-owned; false for the primary template.
        static constexpr bool ServerOwned = false;
    };

    /// @brief Primary template authoring whether an entity carrying this type is always network-relevant.
    ///
    /// TypeRegistry::Register<T>() reads VengAlwaysRelevant<T>::AlwaysRelevant into
    /// TypeInfo::AlwaysRelevant. Interest management (the per-connection relevancy filter) treats an
    /// entity as relevant to every connection regardless of distance when it carries any
    /// always-relevant component — the escape hatch for global game state (the session, seats) that a
    /// spatial query would otherwise cull. A separate specialisation point from VengReflect<T>, like
    /// VengReplication<T>, so it composes with every reflection macro. False unless VE_ALWAYS_RELEVANT
    /// marks it.
    /// @tparam T  The type whose always-relevant default is authored.
    template <class T>
    struct VengAlwaysRelevant
    {
        /// @brief Whether an entity with this component is always relevant; false for the primary template.
        static constexpr bool AlwaysRelevant = false;
    };

    /// @brief Primary template authoring whether a type is a view/presentation output; false unless VE_VIEW_OUTPUT marks it.
    ///
    /// TypeRegistry::Register<T>() reads VengViewOutput<T>::ViewOutput into TypeInfo::ViewOutput. A
    /// ViewOutput component is derived, view-owned state a GuiOverlay driver (see Veng/Gui/Driver.h)
    /// is permitted to write — the checkable half of the driver boundary. A separate specialisation
    /// point from VengReflect<T>, like VengReplication<T>, so it composes with every reflection macro.
    /// False unless VE_VIEW_OUTPUT marks it.
    /// @tparam T  The type whose view-output default is authored.
    template <class T>
    struct VengViewOutput
    {
        /// @brief Whether the type is a view/presentation output; false for the primary template.
        static constexpr bool ViewOutput = false;
    };

    /// @brief Primary template authoring the sibling components a type requires; empty unless VE_REQUIRES marks it.
    ///
    /// TypeRegistry::Register<T>() reads VengRequires<T>::Required() into TypeInfo::Requires. A
    /// required sibling is one this component resolves off its own entity to do its work, so
    /// Scene::RemoveComponent refuses to remove a component while a live requirer sits beside it —
    /// the dependent never resolves a sibling that has gone. A separate specialisation point from
    /// VengReflect<T>, like VengReplication<T>, so it composes with every reflection macro. Empty
    /// unless VE_REQUIRES marks it.
    /// @tparam T  The type whose required siblings are authored.
    template <class T>
    struct VengRequires
    {
        /// @brief The required siblings' TypeIds; empty for the primary template.
        ///
        /// A member template on a defaulted parameter, like VengReflect's accessors: the vector it
        /// builds is then instantiated only where Register<T>() calls it.
        template <class = void>
        static vector<TypeId> Required()
        {
            return {};
        }
    };

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
        /// @brief The type's dense position in its registry: 0 for the first type registered, counting up.
        ///
        /// A Scene indexes its component pools by it, so finding a pool is an array index rather
        /// than a hash. The registry assigns it on insertion, so every scene built from one registry
        /// agrees on it; another registry may number the same type differently.
        u32 Ordinal = InvalidTypeOrdinal;
        /// @brief The meta-kind — Struct for components, others for leaves.
        FieldClass Class = FieldClass::Struct;
        /// @brief Whether the type replicates over the wire, authored via VE_REPLICATED.
        ///
        /// The net layer's snapshot encoder walks only the pools of Replicated types; a type
        /// replicates whole (there is no per-field filtering — a type with a client-local field
        /// splits it out). False for every unmarked type. Set from VengReplication<T>::Replicated.
        bool Replicated = false;
        /// @brief Whether the replicated type is server-owned — authoritative on every peer, never predicted — via VE_SERVER_OWNED.
        ///
        /// A peer predicting an entity that carries it applies the snapshot's value (and presence)
        /// directly, excludes it from the reconciliation compare, leaves it out of the prediction
        /// history, and holds it across a replay. Always false when Replicated is false. Set from
        /// VengServerOwned<T>::ServerOwned.
        bool ServerOwned = false;
        /// @brief Whether an entity carrying this component is always network-relevant, via VE_ALWAYS_RELEVANT.
        ///
        /// Interest management skips the spatial cull for an entity with any always-relevant
        /// component (the session, seats), so global game state reaches every connection regardless
        /// of distance. False for every unmarked type. Set from VengAlwaysRelevant<T>::AlwaysRelevant.
        bool AlwaysRelevant = false;
        /// @brief Whether the type is a view/presentation output a GuiOverlay driver may write, via VE_VIEW_OUTPUT.
        ///
        /// A ViewOutput component is derived, view-owned state gameplay may read but no simulation or
        /// wire owns — the one class of component (beyond request/command components) a driver is
        /// permitted to write. False for every unmarked type. Set from VengViewOutput<T>::ViewOutput.
        bool ViewOutput = false;
        /// @brief The sibling components an entity carrying this type must keep, authored via VE_REQUIRES.
        ///
        /// Scene::RemoveComponent refuses to remove a component named here from an entity that also
        /// carries this one, and names this type as the requirer — so a component resolving a
        /// sibling off its entity cannot be left resolving one that has gone. It constrains removal
        /// only: an entity mid-assembly may carry this component before its siblings. Empty for
        /// every unmarked type. Set from VengRequires<T>::Required().
        vector<TypeId> Requires;
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
    class VE_API TypeRegistry
    {
    public:
        /// @brief Constructs an empty registry.
        TypeRegistry();

        /// @brief Destroys the registry and the storage it owns.
        ~TypeRegistry();

        /// @brief Move-constructs, taking over the source registry's storage.
        TypeRegistry(TypeRegistry&& other) noexcept;

        /// @brief Move-assigns, taking over the source registry's storage.
        TypeRegistry& operator=(TypeRegistry&& other) noexcept;

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

            if (IsRegistered(id))
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

        /// @brief Returns T's ordinal in this registry, or InvalidTypeOrdinal when T is not registered.
        ///
        /// The per-access pool lookup a Scene makes: a hit in T's cache costs one relaxed atomic load
        /// and a compare, so a component access never hashes while one registry is in use. A miss
        /// (first use, or a different registry used last) resolves through OrdinalOf(TypeId) and
        /// refills the cache. Safe to call from several threads.
        /// @tparam T  The type to look up.
        /// @return T's ordinal, or InvalidTypeOrdinal.
        template <class T>
        [[nodiscard]] u32 OrdinalOf() const
        {
            std::atomic<u64>& cache = Detail::g_TypeOrdinalCache<T>;
            const u64 cached = cache.load(std::memory_order_relaxed);
            if (static_cast<u32>(cached >> 32) == m_Serial)
            {
                return static_cast<u32>(cached);
            }
            const u32 ordinal = OrdinalOf(TypeIdOf<T>());
            // An unregistered type is not cached, so registering it later is seen.
            if (ordinal != InvalidTypeOrdinal)
            {
                cache.store((u64{m_Serial} << 32) | ordinal, std::memory_order_relaxed);
            }
            return ordinal;
        }

        /// @brief Returns the ordinal of the type registered under @p id, or InvalidTypeOrdinal when none is.
        /// @param id  The TypeId to look up.
        /// @return The type's ordinal, or InvalidTypeOrdinal.
        [[nodiscard]] u32 OrdinalOf(TypeId id) const;

        /// @brief Returns the TypeInfo for the given id; fatal assert if not registered.
        [[nodiscard]] const TypeInfo& Info(TypeId id) const;

        /// @brief Returns true if the given TypeId has been registered.
        [[nodiscard]] bool IsRegistered(TypeId id) const;

        /// @brief Returns the number of registered types.
        [[nodiscard]] usize Count() const;

        /// @brief Read-only view over every registered (id, info) pair.
        ///
        /// For tooling that enumerates the table (a reflected type manifest, an
        /// editor type picker). Iteration order is unspecified.
        [[nodiscard]] const unordered_map<TypeId, TypeInfo>& All() const;

    private:
        /// @brief Records `info` under `id`, aborting when the id is already claimed.
        ///
        /// The one non-template insertion point, so RegisterImpl's per-type instantiation
        /// carries no map operation into the calling translation unit.
        /// @param id    The authored TypeId to record under.
        /// @param info  The synthesised record.
        void Insert(TypeId id, TypeInfo info);

        /// @brief Synthesises T's lifecycle thunks and inserts a TypeInfo under id; asserts on collision.
        template <class T>
        TypeId RegisterImpl(TypeId id, string name, FieldClass cls, vector<FieldDescriptor> fields)
        {
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
            info.Requires = VengRequires<T>::Required();
            static_assert(!VengServerOwned<T>::ServerOwned || VengReplication<T>::Replicated,
                          "VE_SERVER_OWNED requires VE_REPLICATED on the same type");
            info.Replicated = VengReplication<T>::Replicated;
            info.ServerOwned = VengServerOwned<T>::ServerOwned;
            info.AlwaysRelevant = VengAlwaysRelevant<T>::AlwaysRelevant;
            info.ViewOutput = VengViewOutput<T>::ViewOutput;

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

            Insert(id, std::move(info));
            return id;
        }

        /// @brief The registry's type table, defined in the implementation TU.
        struct Impl;

        /// @brief The owned storage, held by pointer so an including TU sees no table.
        Unique<Impl> m_Impl;

        /// @brief This registry's process-unique identity, which keys the per-type ordinal caches.
        ///
        /// Never zero. A move hands it to the destination and gives the source a fresh one, so a
        /// cache entry never matches a registry whose table it was not resolved against.
        u32 m_Serial;
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

    /// @brief Finds a registered type by the fully-qualified spelling authored JSON names it with.
    ///
    /// The lookup behind every authored type reference — a variant alternative's `"type"` tag, a
    /// table column's `"type"` — so one spelling resolves a reflected type everywhere. Matching is
    /// TypeNameMatches: strict, tolerating only a leading "::".
    /// @param registry  The registry to search.
    /// @param name      The authored fully-qualified spelling.
    /// @return The matching type's info, or nullptr when no registered type carries that name.
    [[nodiscard]] VE_API const TypeInfo* FindTypeByName(const TypeRegistry& registry,
                                                        std::string_view name);
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
        template <class = void>                                                                    \
        static ::Veng::vector<::Veng::FieldDescriptor> Fields()                                    \
        {                                                                                          \
            return {};                                                                             \
        }                                                                                          \
        template <class = void>                                                                    \
        static void RegisterDependencies(::Veng::TypeRegistry&)                                    \
        {                                                                                          \
        }                                                                                          \
    }
