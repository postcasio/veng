#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    /// @brief Stable, authored type identifier — the AssetId discipline applied to C++ types.
    ///
    /// Engine builtins carry a hardcoded 0x…ULL literal checked into the source;
    /// game types mint their own with `vengc generate-id`. Because the id is a
    /// literal, not a compiler type-hash, it is a compile-time constant and
    /// byte-identical across the dlopen boundary.
    using TypeId = u64;

    /// @brief Reserved invalid id; every minted id is non-zero.
    inline constexpr TypeId InvalidTypeId = 0;

    /// @brief The closed meta-kind a generic walker switches on.
    ///
    /// Unlike the open TypeId space (any game type adds a new id with no engine
    /// change), this set is fixed: the serializer and editor inspector share
    /// exactly these cases. Reference is an intra-scene Entity reference the
    /// prefab loader remaps; Struct recurses into the field type's own Fields.
    enum class FieldClass : u8
    {
        /// @brief A plain scalar (bool, f32, i32, u32, u64).
        Scalar,
        /// @brief A glm vector (vec2, vec3, vec4).
        Vector,
        /// @brief A glm quaternion (quat).
        Quaternion,
        /// @brief A glm matrix (mat4).
        Matrix,
        /// @brief A std::string / Veng::string.
        String,
        /// @brief An AssetHandle\<T\> — the serialized form is its leading u64 AssetId.
        AssetHandle,
        /// @brief An intra-scene Entity reference remapped by the prefab loader.
        Reference,
        /// @brief A nested struct — recurses into the field type's own Fields.
        Struct,
        /// @brief An enum whose underlying type is a registered leaf.
        Enum,
        /// @brief A tagged union — at most one of several registered alternatives, reached through the TypeInfo variant ops.
        Variant,
    };
}
