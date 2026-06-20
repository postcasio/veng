#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>
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

    /// @brief Appends obj's fields to out per the type's descriptors.
    /// @param out      Destination buffer; fields are appended.
    /// @param obj      Pointer to the source value.
    /// @param type     TypeInfo carrying the field descriptors.
    /// @param registry Registry used to resolve nested struct field types.
    VE_API void WriteFields(vector<u8>& out, const void* obj, const TypeInfo& type,
                            const TypeRegistry& registry);

    /// @brief Reads fields from in into obj per the descriptors, tolerating schema drift in either direction.
    ///
    /// A truncated record (a length prefix or value that runs past the end of `in`)
    /// is a recoverable error returned as an error string. A descriptor that names
    /// a type the registry does not hold is a schema/registration fault and a fatal assert.
    /// @param in       Source byte span (name-keyed, length-prefixed field records).
    /// @param obj      Pointer to the destination value (default-constructed by the caller).
    /// @param type     TypeInfo carrying the field descriptors.
    /// @param registry Registry used to resolve nested struct field types.
    /// @return         Empty on success; an error string on truncation or format error.
    VE_API VoidResult ReadFields(std::span<const u8> in, void* obj, const TypeInfo& type,
                                 const TypeRegistry& registry);
}
