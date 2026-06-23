#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    /// @brief A world-space ray: an origin and a direction.
    ///
    /// glm-only value type (copied freely like AABB/vec3, no ownership rule),
    /// so it stays inside the public/backend include-hygiene split. The producer
    /// (Viewport::ScreenToWorldRay) normalizes Direction, but the struct imposes
    /// no invariant: At scales Direction as given, so a non-unit Direction
    /// parametrizes t in units of its length.
    struct Ray
    {
        /// @brief The point the ray starts from.
        vec3 Origin;
        /// @brief The direction the ray travels.
        vec3 Direction;

        /// @brief Returns the point at parameter t along the ray (Origin + Direction * t).
        ///
        /// At(0) is the origin; with a unit Direction, At(t) advances by t world units.
        /// @param t  Distance along Direction.
        /// @return The point Origin + Direction * t.
        [[nodiscard]] vec3 At(f32 t) const { return Origin + Direction * t; }
    };
}
