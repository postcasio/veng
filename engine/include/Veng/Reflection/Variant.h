#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeId.h>

#include <array>
#include <span>
#include <variant>

namespace Veng
{
    class TypeRegistry;

    /// @brief A reflected tagged union: holds at most one of the alternative types Ts.
    ///
    /// Default-constructs empty (no active alternative), so a freshly-added component or
    /// an omitted prefab field is "no shape", not "shape 0". Reflection reaches the active
    /// member through the type-erased ops VE_VARIANT records on this type's TypeInfo; it
    /// never indexes the storage by offset. Each Ts must be a registered Struct-class
    /// reflected type. std::variant supplies move/destruct, so a Variant member is a
    /// normal poolable value, not a heap-owning one.
    /// @tparam Ts The alternative types, in declaration order.
    template <class... Ts>
    class Variant
    {
    public:
        /// @brief Constructs an empty variant (no active alternative).
        Variant() = default;

        /// @brief Returns the active alternative's TypeId, or InvalidTypeId when empty.
        /// @return The active alternative's TypeId, InvalidTypeId for the empty state.
        [[nodiscard]] TypeId ActiveType() const
        {
            return std::visit(
                [](const auto& value) -> TypeId
                {
                    using Held = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Held, std::monostate>)
                    {
                        return InvalidTypeId;
                    }
                    else
                    {
                        return TypeIdOf<Held>();
                    }
                },
                m_Storage);
        }

        /// @brief Returns true when an alternative is active.
        [[nodiscard]] bool HasValue() const { return m_Storage.index() != 0; }

        /// @brief Activates the alternative whose TypeId is `id`, default-constructed.
        ///
        /// An id matching none of Ts leaves the variant untouched; the caller treats the
        /// null return as a skip / located error.
        /// @param id The TypeId of the alternative to activate.
        /// @return Pointer to the activated member's storage, or nullptr if `id` is none of Ts.
        void* SetActive(TypeId id)
        {
            // The result is the writable storage the caller mutates, so the pointee
            // stays non-const despite never being written through inside this function.
            void* result = nullptr; // NOLINT(misc-const-correctness)
            (
                [&]
                {
                    if (result == nullptr && id == TypeIdOf<Ts>())
                    {
                        result = &m_Storage.template emplace<Ts>();
                    }
                }(),
                ...);
            return result;
        }

        /// @brief Resets to the empty (monostate) state, destructing any active alternative.
        void Clear() { m_Storage.template emplace<std::monostate>(); }

        /// @brief Returns the active member's storage, or nullptr when empty.
        /// @return Pointer to the active member, nullptr for the empty state.
        [[nodiscard]] void* ActivePtr()
        {
            return std::visit(
                [](auto& value) -> void*
                {
                    using Held = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Held, std::monostate>)
                    {
                        return nullptr;
                    }
                    else
                    {
                        return &value;
                    }
                },
                m_Storage);
        }

        /// @brief Returns the active member's storage, or nullptr when empty.
        /// @return Const pointer to the active member, nullptr for the empty state.
        [[nodiscard]] const void* ActivePtr() const
        {
            return std::visit(
                [](const auto& value) -> const void*
                {
                    using Held = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Held, std::monostate>)
                    {
                        return nullptr;
                    }
                    else
                    {
                        return &value;
                    }
                },
                m_Storage);
        }

        /// @brief The TypeIds of the alternatives, in declaration order.
        /// @return A span over a static array of the alternatives' TypeIds.
        [[nodiscard]] static std::span<const TypeId> Alternatives()
        {
            static constexpr std::array<TypeId, sizeof...(Ts)> Ids{TypeIdOf<Ts>()...};
            return Ids;
        }

        /// @brief Registers every alternative type into `registry` (the dependency recursion).
        /// @param registry The registry receiving each alternative's registration.
        static void RegisterAlternatives(TypeRegistry& registry)
        {
            (registry.template Register<Ts>(), ...);
        }

    private:
        /// @brief The tagged storage; index 0 (monostate) is the empty state.
        std::variant<std::monostate, Ts...> m_Storage;
    };
}
