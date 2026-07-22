#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>

#include <nlohmann/json.hpp>

namespace Veng
{
    class TypeRegistry;
    class AssetManager;
    struct TypeInfo;
}

namespace Veng::Mcp
{
    /// @brief Emits a reflected value's fields as a JSON object, keyed by serialization name.
    ///
    /// A thin wrapper over the shared Veng::JsonWriteFields walker: MCP supplies its own
    /// entity-addressing hooks (a Reference field reads/writes `{ index, generation }`) and
    /// re-encodes each AssetHandle leaf from the walker's raw integer to a decimal string, so a
    /// 64-bit AssetId round-trips exactly through a JSON-number client. This is the canonical MCP
    /// component encoding, reused by every tool that dumps a component.
    ///
    /// Per FieldClass: Scalar → number/bool; Vector/Quaternion → array; Matrix → nested array;
    /// String → string; Enum → the bare enumerator name string; AssetHandle → the referenced
    /// AssetId as a decimal string; Reference → the referenced entity's { index, generation };
    /// Struct → a recursed object; Variant → { type, value }; Array → a JSON array.
    ///
    /// This is an internal library header (it names nlohmann::json), never part of the
    /// Veng/Mcp/ public surface.
    /// @param obj       Pointer to the value to walk.
    /// @param type      TypeInfo carrying the field descriptors.
    /// @param registry  Registry used to resolve nested/leaf/element types.
    /// @return A JSON object of { <field name>: <value> }.
    nlohmann::json FieldsToJson(const void* obj, const TypeInfo& type,
                                const TypeRegistry& registry);

    /// @brief Writes a JSON object's fields into a reflected value's bytes, keyed by serialization name.
    ///
    /// The inverse of FieldsToJson: a thin wrapper over the shared Veng::JsonReadFields walker,
    /// with MCP's entity-addressing hooks and the AssetHandle decimal-string decode ahead of the
    /// walk. `allowUnknownFields` is always on (a source key naming no field is skipped), matching
    /// this library's existing tolerant posture. This is the canonical MCP write-side component
    /// decoding, reused by every mutation tool.
    ///
    /// The update is **partial and tolerant**: a field the descriptors have but @p source omits
    /// keeps its current value. A value whose JSON kind does not match the field's class (a
    /// string where a number is required, an object where an array is required) is a located
    /// error, not a skip — a malformed request is reported, never silently ignored. An enum field
    /// reads a bare enumerator name string only; an integer (or the old { value, name } object)
    /// is a located error, the same hard cut every JSON surface in the tree enforces.
    ///
    /// Per FieldClass: Scalar ← number/bool; Vector/Quaternion/Matrix ← array; String ← string;
    /// Enum ← the enumerator name string; AssetHandle ← an AssetId (decimal string or number);
    /// Reference ← { index, generation }; Struct ← a recursed object; Variant ← { type, value };
    /// Array ← a JSON array via the resize/element shims.
    ///
    /// This is an internal library header (it names nlohmann::json), never part of the
    /// Veng/Mcp/ public surface.
    /// @param source    The JSON object of { <field name>: <value> } to apply (a partial update).
    /// @param obj       Pointer to the value to write into.
    /// @param type      TypeInfo carrying the field descriptors.
    /// @param registry  Registry used to resolve nested/leaf/element/variant types.
    /// @return Empty on success; a located error string on a type mismatch or an unresolvable type.
    VoidResult JsonToFields(const nlohmann::json& source, void* obj, const TypeInfo& type,
                            const TypeRegistry& registry);
}
