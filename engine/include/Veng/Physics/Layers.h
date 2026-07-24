#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>

#include <array>

namespace Veng
{
    /// @brief The engine's closed collision-layer table.
    ///
    /// Every body sits on exactly one layer, and a CollisionMatrix decides which pairs of layers
    /// are allowed to produce contacts. The set is deliberately **closed**: filtering is what
    /// silently breaks a world when it drifts, and a table that fits on one screen is worth more
    /// than an extensible one. A consumer selects a matrix over these layers; it never adds a
    /// layer.
    ///
    /// Integer values are stable — persisted in prefabs.
    enum class PhysicsLayer : u32
    {
        /// @brief Immovable world geometry: terrain, walls, structure. Never simulated.
        Static = 0,
        /// @brief Ordinary movable bodies — dynamic props and kinematic movers.
        Moving = 1,
        /// @brief Player and creature bodies, separated so a matrix can treat them apart from props.
        Character = 2,
        /// @brief Sensor volumes: they report overlaps and never push anything.
        Trigger = 3,
        /// @brief Bodies that exist only to be found by queries — they collide with nothing.
        Query = 4,
    };

    /// @brief Number of members in the closed PhysicsLayer table.
    inline constexpr u32 PhysicsLayerCount = 5;

    /// @brief Which layer pairs may produce contacts, as one bitmask row per layer.
    ///
    /// Row `L` has bit `M` set when a body on layer `L` collides with a body on layer `M`. The
    /// matrix is required to be symmetric — the solver consults it in one direction only, so an
    /// asymmetric table means "a hits b but b does not hit a", which no solver can honour.
    /// IsSymmetric() checks that; PhysicsWorld::Create asserts it.
    struct CollisionMatrix
    {
        /// @brief One bitmask row per PhysicsLayer, indexed by the layer's integer value.
        std::array<u32, PhysicsLayerCount> Rows{};
    };

    /// @brief The bit a layer occupies in a CollisionMatrix row.
    /// @param layer  The layer whose bit to compute.
    /// @return A single-bit mask.
    [[nodiscard]] constexpr u32 PhysicsLayerBit(const PhysicsLayer layer)
    {
        return 1U << static_cast<u32>(layer);
    }

    /// @brief Whether two layers may produce contacts under @p matrix.
    /// @param matrix  The table to consult.
    /// @param a       One layer.
    /// @param b       The other layer.
    /// @return True when a body on @p a collides with a body on @p b.
    [[nodiscard]] constexpr bool LayersCollide(const CollisionMatrix& matrix, const PhysicsLayer a,
                                               const PhysicsLayer b)
    {
        return (matrix.Rows[static_cast<usize>(a)] & PhysicsLayerBit(b)) != 0;
    }

    /// @brief Whether every entry of @p matrix agrees with its transpose.
    /// @param matrix  The table to check.
    /// @return True when the table is symmetric.
    [[nodiscard]] constexpr bool IsSymmetric(const CollisionMatrix& matrix)
    {
        for (u32 a = 0; a < PhysicsLayerCount; ++a)
        {
            for (u32 b = 0; b < PhysicsLayerCount; ++b)
            {
                const bool forward = (matrix.Rows[a] & (1U << b)) != 0;
                const bool backward = (matrix.Rows[b] & (1U << a)) != 0;
                if (forward != backward)
                {
                    return false;
                }
            }
        }
        return true;
    }

    /// @brief The matrix a PhysicsWorldInfo selects unless the consumer names another.
    ///
    /// Static geometry stops movers and characters but not other static geometry; movers,
    /// characters and triggers all see each other; Query bodies collide with nothing, which is
    /// what makes them query-only.
    /// @return The default symmetric table.
    [[nodiscard]] constexpr CollisionMatrix DefaultCollisionMatrix()
    {
        constexpr u32 StaticBit = PhysicsLayerBit(PhysicsLayer::Static);
        constexpr u32 MovingBit = PhysicsLayerBit(PhysicsLayer::Moving);
        constexpr u32 CharacterBit = PhysicsLayerBit(PhysicsLayer::Character);
        constexpr u32 TriggerBit = PhysicsLayerBit(PhysicsLayer::Trigger);

        CollisionMatrix matrix;
        matrix.Rows[static_cast<usize>(PhysicsLayer::Static)] = MovingBit | CharacterBit;
        matrix.Rows[static_cast<usize>(PhysicsLayer::Moving)] =
            StaticBit | MovingBit | CharacterBit | TriggerBit;
        matrix.Rows[static_cast<usize>(PhysicsLayer::Character)] =
            StaticBit | MovingBit | CharacterBit | TriggerBit;
        matrix.Rows[static_cast<usize>(PhysicsLayer::Trigger)] = MovingBit | CharacterBit;
        matrix.Rows[static_cast<usize>(PhysicsLayer::Query)] = 0;
        return matrix;
    }
}

VE_ENUM(::Veng::PhysicsLayer, 0x6590333E5A1B792DULL)
VE_ENUMERATOR(Static)
VE_ENUMERATOR(Moving)
VE_ENUMERATOR(Character)
VE_ENUMERATOR(Trigger)
VE_ENUMERATOR(Query)
VE_ENUM_END();
