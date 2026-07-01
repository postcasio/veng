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
    /// The read-side analogue of the editor inspector's DrawFieldWidget and the cooker's
    /// JSON → field parse: it walks the type's FieldDescriptors and emits each field by its
    /// closed FieldClass, keyed by the serialization Name (never the display label — the
    /// on-disk field identity). This is the canonical MCP component encoding, reused by
    /// every tool that dumps a component.
    ///
    /// Per FieldClass: Scalar → number/bool; Vector/Quaternion → array; Matrix → nested
    /// array; String → string; Enum → the enumerator name (with the raw integer alongside);
    /// AssetHandle → the referenced AssetId as a decimal string; Reference → the referenced
    /// entity's { index, generation }; Struct → a recursed object; Variant → { type, value };
    /// Array → a JSON array. It tolerates schema drift the way the serializer does: an
    /// unregistered nested/element type is reported inline rather than dereferenced.
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
    /// The inverse of FieldsToJson and the JSON analogue of the binary ReadFields: it walks
    /// the type's FieldDescriptors and, for each key present in @p source, parses the value by
    /// the field's closed FieldClass into the field's storage. This is the canonical MCP
    /// write-side component decoding, reused by every mutation tool.
    ///
    /// The update is **partial and tolerant**: a field the descriptors have but @p source omits
    /// keeps its current value, and a @p source key naming no field is skipped — the serializer's
    /// schema-drift tolerance, so a mutation touches only the fields it names. A value whose JSON
    /// kind does not match the field's class (a string where a number is required, an object where
    /// an array is required) is a located error, not a skip — a malformed request is reported,
    /// never silently ignored.
    ///
    /// Every agent-supplied type name it resolves (a Variant's active-type QualifiedName, an enum
    /// enumerator) goes through a fallible lookup and yields a located error on a miss — it never
    /// reaches an asserting registry.Info(). The Array arm resizes to the incoming array's element
    /// count, clamped to a sanity cap so a malformed count cannot trigger a pathological allocation.
    ///
    /// Per FieldClass: Scalar ← number/bool; Vector/Quaternion/Matrix ← array; String ← string;
    /// Enum ← the enumerator name or the raw integer; AssetHandle ← an AssetId (decimal string or
    /// number); Reference ← { index, generation }; Struct ← a recursed object; Variant ←
    /// { type, value }; Array ← a JSON array via the resize/element shims.
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
