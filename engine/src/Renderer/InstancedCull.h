#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Asset/MeshLod.h>
#include <Veng/Math/AABB.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Renderer/InstancedMesh.h>

#include "LodSelection.h"

namespace Veng::Renderer
{
    /// @brief The level-memory value meaning "no level was drawn last frame".
    inline constexpr u8 InstanceNoPreviousLod = 0xFF;

    /// @brief What the per-instance cull needs of one instanced mesh set.
    struct InstancedCullSet
    {
        /// @brief The set's frame: set space -> world.
        mat4 World{1.0f};
        /// @brief The mesh's level-0 bound, in its object space.
        AABB MeshBounds = AABB::Empty();
        /// @brief The mesh's level switch thresholds (Mesh::GetLodScreenSizes), descending.
        std::span<const f32> LodThresholds;
        /// @brief The levels the mesh carries.
        u32 LodCount = 1;
        /// @brief The projected-size band in the reference view, in pixels.
        f32 MinPixels = 0.0f;
        /// @brief The band's upper end.
        f32 MaxPixels = 0.0f;
        /// @brief A shadow view skips an instance spanning fewer of its texels.
        f32 MinShadowTexels = 0.0f;
        /// @brief The set's forced level (its entity's LodOverride), or AutomaticLod.
        i32 ForcedLod = AutomaticLod;
    };

    /// @brief What the per-instance cull needs of one view.
    struct InstancedCullView
    {
        /// @brief The view's frustum, in world space.
        Frustum Planes{};
        /// @brief Whether instances outside Planes are culled.
        bool FrustumCull = true;
        /// @brief The main camera: the only view keeping level memory and testing occlusion.
        bool Main = false;
        /// @brief How the view measures size for level selection (bias and screen scale applied).
        LodView Lod;
        /// @brief The level band's half-width; 0 for a view with no level memory.
        f32 Hysteresis = 0.0f;
        /// @brief A shadow view's tile size in texels.
        f32 Resolution = 0.0f;
        /// @brief The view's forced level, or AutomaticLod.
        i32 ForcedLod = AutomaticLod;
    };

    /// @brief The view an instanced set's projected-size band is measured in: the main camera.
    struct InstancedCullReference
    {
        /// @brief The main camera's unjittered world -> clip matrix.
        mat4 ViewProj{1.0f};
        /// @brief The main camera's position.
        vec3 Eye{0.0f};
        /// @brief The main camera's rendered height in pixels.
        f32 HeightPixels = 1.0f;
    };

    /// @brief The cull's verdict on one instance in one view.
    struct InstancedCullVerdict
    {
        /// @brief Whether the instance is drawn.
        bool Drawn = false;
        /// @brief The level it draws at, when drawn.
        u32 Lod = 0;
    };

    /// @brief Decides one instance in one view: frustum, size band, shadow caster size, level.
    ///
    /// The host twin of instanced_mesh_cull.comp's per-invocation decision, minus the occlusion
    /// test (the host has no pyramid). The CPU fallback runs it per instance per view, and the
    /// device-free tests pin its rules.
    /// @param instance The instance.
    /// @param set      Its set.
    /// @param view     The view.
    /// @param reference The main camera, for the size band.
    /// @param previous The level this instance drew at in this view last frame, or
    ///                 InstanceNoPreviousLod.
    /// @return Whether it is drawn and at which level.
    [[nodiscard]] InstancedCullVerdict CullInstance(const MeshInstance& instance,
                                                    const InstancedCullSet& set,
                                                    const InstancedCullView& view,
                                                    const InstancedCullReference& reference,
                                                    u8 previous);
}
