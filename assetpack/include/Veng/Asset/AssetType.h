#pragma once

#include <Veng/Asset/Types.h>

#include <string_view>

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
        /// @brief An equirectangular HDR environment map for image-based lighting (see CookedEnvironmentHeader).
        Environment,
        /// @brief A parameter override over a parent Material (see CookedMaterialInstanceHeader).
        MaterialInstance,
    };

    /// @brief Canonical authoring/manifest name of an asset type ("texture", "material_instance", …).
    ///
    /// The single spelling the pack manifest and every tool agree on. The inverse of
    /// @ref ParseAssetType. An unmapped value returns "unknown".
    /// @param type  The asset type.
    /// @return A static, never-null name string.
    [[nodiscard]] const char* ToString(AssetType type);

    /// @brief Parses a canonical authoring/manifest type name into an AssetType.
    ///
    /// The inverse of @ref ToString; the one place the manifest's "type" string is decoded,
    /// shared by the cooker and the editor's source index.
    /// @param name  The manifest type name (e.g. "material_instance").
    /// @return The matching type, or nullopt when the name is unrecognized.
    [[nodiscard]] optional<AssetType> ParseAssetType(std::string_view name);
}
