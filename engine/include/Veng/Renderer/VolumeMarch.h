#pragma once

#include <optional>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>

/// @brief Pure, device-free math for the volume-field ray-march pass.
///
/// The CPU counterparts of the per-pixel work the volume march shader does: the ray/AABB segment
/// clipped against the reconstructed scene depth (mirrored by the shader so the same rule governs
/// which pixels march), the far-to-near draw ordering the multi-field composite relies on, and the
/// resolved per-field draw record. glm-only value types, no GPU — unit-testable with no ICD.
namespace Veng::Renderer
{
    class VolumeField;

    /// @brief The world-space ray segment a volume march integrates over: entry/exit ray parameters.
    ///
    /// Enter/Exit are parameters t along the ray origin + t·dir, in the units of a normalized dir
    /// (i.e. world distance). The march samples the field between them; a caller with the empty
    /// segment (Exit <= Enter) marches nothing.
    struct MarchSegment
    {
        /// @brief Ray parameter where the segment enters the field (clamped to the origin, >= 0).
        f32 Enter = 0.0f;
        /// @brief Ray parameter where the segment exits (clamped to the nearest opaque surface).
        f32 Exit = 0.0f;
    };

    /// @brief Computes the world-space march segment of a ray through a volume field's AABB.
    ///
    /// A slab intersection of the ray (origin + t·dir) with bounds, then two clamps: the entry is
    /// floored at the origin (t >= 0, so a camera inside the box marches from 0), and the exit is
    /// capped at maxDistance (the reconstructed scene depth along the ray, so opaque geometry in
    /// front of the far face shortens the march and geometry in front of the near face occludes it
    /// entirely). Returns nullopt when the ray misses the box or the surviving segment is empty — a
    /// missed, grazing, or fully occluded field contributes nothing and the shader discards.
    /// @param origin      Ray origin in world space (the camera position).
    /// @param dir         Ray direction in world space, normalized (t is then world distance).
    /// @param bounds      The field's world-space AABB.
    /// @param maxDistance Ray parameter of the nearest opaque surface along the ray (scene depth),
    ///                    or a large value (the far plane) for the cleared background.
    /// @return The [Enter, Exit] segment, or nullopt on a miss / empty segment.
    [[nodiscard]] std::optional<MarchSegment>
    ComputeMarchSegment(vec3 origin, vec3 dir, const AABB& bounds, f32 maxDistance);

    /// @brief Orders two fields far-to-near by camera distance to their bounds centers.
    ///
    /// Returns true when box a's center is strictly farther from cameraPos than b's (squared
    /// distance). Used as the strict-weak "less" predicate, it sorts the per-frame draw list
    /// back-to-front: the farthest field draws first, and each nearer field's (ONE, SRC_ALPHA)
    /// blend attenuates whatever the farther fields already composited behind it.
    /// @param cameraPos The camera world position distances are measured from.
    /// @param a         The first field's world-space AABB.
    /// @param b         The second field's world-space AABB.
    /// @return True if a is farther from the camera than b.
    [[nodiscard]] bool VolumeFieldFartherFirst(vec3 cameraPos, const AABB& a, const AABB& b);

    /// @brief One live volume field resolved for a frame: the built resource plus its authored knobs.
    ///
    /// The renderer refills a list of these each Execute from the scene's VolumeField components (the
    /// lights model) and hands it to the pass, which draws one fullscreen march per entry far-to-near.
    /// Opacity/EmissionScale/ExtinctionScale/Steps are the component's authored knobs; Field is the
    /// built GPU resource the march samples.
    struct VolumeFieldInstance
    {
        /// @brief The built GPU field this instance draws (never null in a resolved list).
        const VolumeField* Field = nullptr;
        /// @brief Overall fade scaling emission and extinction toward zero, in [0, 1].
        f32 Opacity = 1.0f;
        /// @brief Unit remap over the baked emission radiance density.
        f32 EmissionScale = 1.0f;
        /// @brief Unit remap over the baked extinction density.
        f32 ExtinctionScale = 1.0f;
        /// @brief Fixed ray-march step count through the field.
        u32 Steps = 64;
    };
}
