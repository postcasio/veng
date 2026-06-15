#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    // A generational handle into a Scene's entity table. The generation bumps
    // every time a slot is recycled by DestroyEntity, so a handle whose slot
    // was reused is detectably stale — accessing a component through one is API
    // misuse and a fatal VE_ASSERT, never silent UB. Entity::Null is the empty
    // handle.
    struct Entity
    {
        static constexpr u32 InvalidIndex = ~0u;

        u32 Index = InvalidIndex;
        u32 Generation = 0;

        static const Entity Null;

        [[nodiscard]] bool IsNull() const { return Index == InvalidIndex; }

        bool operator==(const Entity&) const = default;
    };

    inline const Entity Entity::Null = Entity{Entity::InvalidIndex, 0};
}

template <>
struct std::hash<Veng::Entity>
{
    Veng::usize operator()(const Veng::Entity& entity) const noexcept
    {
        // Index uniquely identifies a live slot; the generation rides along but
        // the index alone is a fine hash key (one live entity per index).
        return std::hash<Veng::u32>{}(entity.Index);
    }
};
