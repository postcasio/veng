#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>

#include <new>
#include <utility>

namespace Veng
{
    // A stable, authored type identifier — the AssetId discipline applied to
    // C++ types. Engine builtins carry a hardcoded 0x…ULL literal checked into
    // the source; game types mint their own with `vengc generate-id`. Because
    // the id is a literal, not a compiler type-hash, it is a compile-time
    // constant and byte-identical across the eventual dlopen boundary.
    using TypeId = u64;

    // 0 is reserved as the invalid id; every minted id is non-zero.
    inline constexpr TypeId InvalidTypeId = 0;

    // The recorded description of a registered type. This is the lifecycle
    // slice: name, layout, and the construct/destruct/move thunks a type-erased
    // pool drives. The reflection layer adds field descriptors alongside these.
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
    };

    // A type declares its stable TypeId by specialising this trait:
    //   template <> struct VengReflect<MyType> { static constexpr TypeId Id = …; };
    // The VE_TYPE macro writes that specialisation. TypeRegistry::Register<T>
    // and IdOf<T> read Id off it; the reflection layer enriches the same trait.
    template <class T>
    struct VengReflect;

    // Records each registered type under its authored TypeId, keyed by that id.
    // Registration is main-thread, startup-only and must complete before any
    // Scene that pools the types is touched concurrently with task-system
    // workers. The registry is engine-owned (Application::GetTypeRegistry()) and
    // threaded into Scene::Create — no global.
    class TypeRegistry
    {
    public:
        // Synthesises T's lifecycle thunks and records its TypeInfo under
        // VengReflect<T>::Id. Registering two distinct types under the same id
        // is a fatal collision assert. Returns the recorded id.
        template <class T>
        TypeId Register(string name)
        {
            constexpr TypeId id = VengReflect<T>::Id;
            static_assert(id != InvalidTypeId,
                          "VengReflect<T>::Id must be a non-zero authored id");

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

            m_Types.emplace(id, std::move(info));
            return id;
        }

        // The authored TypeId of T, read straight off its trait — a compile-time
        // constant, independent of registration order or this instance.
        template <class T>
        [[nodiscard]] constexpr TypeId IdOf() const
        {
            return VengReflect<T>::Id;
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

    private:
        unordered_map<TypeId, TypeInfo> m_Types;
    };
}

// Declares a type's stable TypeId by specialising VengReflect<T>. The id is an
// authored 0x…ULL literal (engine builtins) or a `vengc generate-id` value
// (game types). The reflection layer's VE_REFLECT macro supersedes this with a
// fielded specialisation; this minimal form carries only the Id.
#define VE_TYPE(Type, TypeIdLiteral)                                            \
    template <>                                                                 \
    struct ::Veng::VengReflect<Type>                                            \
    {                                                                           \
        static constexpr ::Veng::TypeId Id = (TypeIdLiteral);                   \
    }
