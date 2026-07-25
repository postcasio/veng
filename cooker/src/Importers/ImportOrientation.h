#pragma once

#include <Veng/Veng.h>

#include <Veng/Cook/Types.h>

namespace Veng::Cook
{
    /// @brief The rotation reconciling a source model file's axis convention with the engine's.
    ///
    /// veng's model space is **-Z forward, +Y up** — the convention a mesh socket's orientation, a
    /// gameplay heading, and a vehicle's thrust axis are all stated in. A source file authored in a
    /// different convention cannot otherwise be reconciled at import: the mesh and collision
    /// importers accept a uniform `import.scale` but no rotation, so the geometry arrives turned and
    /// every consumer of a direction compensates by hand. Declaring the source's own convention
    /// resolves it once, at cook time, for everything the importer derives from that file.
    ///
    /// Both declared axes are axis-aligned and perpendicular, so the rotation is a **signed
    /// permutation** with determinant +1: every cooked value is an exact sign flip or swap of the
    /// source's, never a resampling, and handedness (a tangent's stored bitangent sign, a
    /// triangle's winding) is preserved rather than needing separate correction.
    struct ImportOrientation
    {
        /// @brief Rotation taking a source-space direction into engine space; identity by default.
        mat3 Rotation{1.0f};

        /// @brief True while the source declares the engine's own convention, so nothing is rotated.
        bool IsIdentity = true;

        /// @brief Rotates a source-space position or direction into engine space.
        /// @param v  The source-space vector.
        /// @return The same vector in engine space, returned verbatim under the default convention.
        [[nodiscard]] vec3 Reorient(const vec3& v) const { return IsIdentity ? v : Rotation * v; }

        /// @brief Rotates a source-space orientation into engine space.
        ///
        /// Composed on the left, so the source's own rotation is applied first and the convention
        /// crossing second — the order that keeps a socket aimed at the same feature of the
        /// geometry it was authored against.
        /// @param q  The source-space orientation.
        /// @return The same orientation in engine space, returned verbatim under the default
        ///         convention.
        [[nodiscard]] quat Reorient(const quat& q) const
        {
            return IsIdentity ? q : glm::normalize(glm::quat_cast(Rotation) * q);
        }
    };

    /// @brief Reads the optional `"orientation"` object out of a source's `"import"` block.
    ///
    /// The object declares the source file's own convention as two axis names — `"forward"` and
    /// `"up"`, each one of `+X`, `-X`, `+Y`, `-Y`, `+Z`, `-Z` (a bare `X` reads as `+X`). Either
    /// key may be omitted and defaults to the engine's own axis, so an absent `"orientation"`, an
    /// empty one, and one restating `-Z` / `+Y` all yield the identity.
    /// @param import  The source's `"import"` object; may be empty.
    /// @return The reconciling rotation, or an error naming the offending key when an axis name is
    ///         unknown or the two axes are not perpendicular.
    [[nodiscard]] Result<ImportOrientation> ParseImportOrientation(const json& import);
}
