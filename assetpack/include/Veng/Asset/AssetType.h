#pragma once

#include <Veng/Asset/Types.h>

namespace Veng
{
    /// @brief Identifies what kind of cooked blob a TOC entry points at.
    ///
    /// Stored in the archive as its underlying u32 (see Archive.h). New types are appended,
    /// never renumbered, so old archives keep decoding correctly.
    enum class AssetType : u32
    {
        /// @brief Untyped raw blob; consumed as-is by the engine.
        Raw = 0,
        /// @brief A decoded texture with sampler parameters (see CookedTextureHeader).
        Texture,
        /// @brief An interleaved vertex + index mesh (see CookedMeshHeader).
        Mesh,
        /// @brief A SPIR-V module with reflected interface (see CookedShaderHeader).
        Shader,
        /// @brief A bindless material referencing vertex and fragment shader assets (see CookedMaterialHeader).
        Material,
        /// @brief A named list of vertex-buffer elements referenced by shaders (see CookedVertexLayoutHeader).
        VertexLayout,
        /// @brief A tree of entities with components, spawnable into a Scene (see CookedPrefabHeader).
        Prefab,
        /// @brief A world prefab plus level-scoped wiring (game mode, system set, render settings) (see CookedLevelHeader).
        Level,
        /// @brief A bone hierarchy with inverse-bind matrices for skinning (see CookedSkeletonHeader).
        Skeleton,
        /// @brief A set of per-bone keyframe tracks animating a skeleton (see CookedAnimationHeader).
        Animation,
    };
}
