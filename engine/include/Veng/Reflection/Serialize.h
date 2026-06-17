#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeRegistry.h>

#include <span>

namespace Veng
{
    // Generic, schema-driven field serialization. Neither function knows any
    // concrete C++ type — they walk a TypeInfo's FieldDescriptors and read/write
    // each field's bytes by FieldClass, so the descriptors alone are enough to
    // save/load any reflected value (a component or any other struct). The cooker
    // produces the cooked prefab blob by reusing WriteFields; the runtime prefab
    // loader reconstructs with ReadFields — one encoder/decoder, shared. There is
    // no on-disk container format here, no assetpack dependency.
    //
    // Within a value, fields are written name-keyed ({ name, value }) and length-
    // prefixed, so ReadFields tolerates schema drift: a record absent from the
    // descriptor is skipped, a descriptor field absent from the data keeps its
    // default. The serializer touches only Name/Type/Class/Offset on each
    // descriptor — the editor metadata never affects the bytes.
    //
    // An AssetHandle field is recorded as its leading u64 AssetId only (the
    // AssetHandle-stores-AssetId-at-offset-0 assumption is pinned by a
    // static_assert in AssetHandle.h); rehydration to a resident handle is the
    // loader's job.

    // Appends obj's fields to out per the type's descriptors.
    VE_API void WriteFields(vector<u8>& out, const void* obj, const TypeInfo& type,
                            const TypeRegistry& registry);

    // Reads fields from in into a (default-constructed) obj per the descriptors,
    // tolerating drift in either direction.
    VE_API void ReadFields(std::span<const u8> in, void* obj, const TypeInfo& type,
                           const TypeRegistry& registry);
}
