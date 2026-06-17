#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    // A stable, authored type identifier — the AssetId discipline applied to
    // C++ types. Engine builtins carry a hardcoded 0x…ULL literal checked into
    // the source; game types mint their own with `vengc generate-id`. Because
    // the id is a literal, not a compiler type-hash, it is a compile-time
    // constant and byte-identical across the eventual dlopen boundary.
    using TypeId = u64;

    // 0 is reserved as the invalid id; every minted id is non-zero.
    inline constexpr TypeId InvalidTypeId = 0;

    // The closed meta-kind a generic walker switches on. Unlike the open TypeId
    // space (any game type adds a new id with no engine change), this set is
    // fixed: the serializer and the future editor inspector share exactly these
    // cases. Reference is an intra-scene Entity reference the future loader
    // remaps; Struct recurses into the field type's own Fields.
    enum class FieldClass : u8
    {
        Scalar,
        Vector,
        Quaternion,
        Matrix,
        String,
        AssetHandle,
        Reference,
        Struct,
        Enum,
    };
}
