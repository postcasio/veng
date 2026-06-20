#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    /// @brief Generational handle into a Scene's entity table.
    ///
    /// The generation bumps every time a slot is recycled by DestroyEntity, so a
    /// handle whose slot was reused is detectably stale — accessing a component
    /// through one is API misuse and a fatal VE_ASSERT, never silent UB.
    /// Entity::Null is the empty handle.
    struct Entity
    {
        /// @brief Sentinel index for null/invalid handles.
        static constexpr u32 InvalidIndex = ~0u;

        /// @brief Slot index into the scene's entity table.
        u32 Index = InvalidIndex;
        /// @brief Generation counter; bumped each time the slot is recycled.
        u32 Generation = 0;

        /// @brief The empty/invalid entity handle.
        static const Entity Null;

        /// @brief Returns true if this handle is null (no entity).
        [[nodiscard]] bool IsNull() const { return Index == InvalidIndex; }

        /// @brief Member-wise equality on index and generation.
        bool operator==(const Entity&) const = default;
    };

    inline const Entity Entity::Null = Entity{.Index = Entity::InvalidIndex, .Generation = 0};
}

/// @brief std::hash specialization for Veng::Entity, keyed on the slot index.
template <>
struct std::hash<Veng::Entity>
{
    /// @brief Hashes an entity by its index (one live entity per index).
    Veng::usize operator()(const Veng::Entity& entity) const noexcept
    {
        return std::hash<Veng::u32>{}(entity.Index);
    }
};
