#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Entity.h>

#include <nlohmann/json_fwd.hpp>

namespace Veng
{
    /// @brief Policy hooks for the consumer-owned parts of JSON field binding.
    ///
    /// The walker's mechanical core (Scalar/Vector/Quaternion/Matrix/String/Enum/Struct/
    /// Variant/Array) is fully generic; only AssetId validation and Entity-reference
    /// (de)serialization are caller-specific, so those alone are policy hooks. A default
    /// JsonFieldHooks accepts every AssetId and errors on any Reference field, matching
    /// the strictest existing fork's posture.
    struct JsonFieldHooks
    {
        /// @brief Validates a nonzero AssetId against the caller's resolve context.
        ///
        /// The prefab importer's cook-time pack-resolve check is the motivating case;
        /// an unset hook accepts every id, deferring validation to load time.
        function<VoidResult(u64 id, TypeId fieldType)> ValidateAssetId;

        /// @brief Maps a JSON value to an Entity for a Reference field.
        ///
        /// A prefab-local index, a live entity lookup, or an MCP addressing scheme are
        /// all caller-specific; an unset hook makes a Reference field a located error.
        function<Result<Entity>(const nlohmann::json& value)> ReadReference;

        /// @brief The write inverse of ReadReference.
        ///
        /// An unset hook makes a Reference field a located error on write, matching
        /// ReadReference's default.
        function<nlohmann::json(Entity entity)> WriteReference;
    };

    /// @brief Binds a name-keyed JSON object into a reflected instance.
    ///
    /// The JSON analogue of ReadFields (Serialize.h): a recursive, name-keyed walk over
    /// `type`'s FieldDescriptors, covering every FieldClass (Scalar / Vector / Quaternion /
    /// Matrix / String / Enum / AssetHandle / Reference / Struct / Variant / Array). An
    /// omitted field keeps its current (caller-constructed) value; a malformed field is a
    /// located error naming the dotted field path (e.g. "Settings.Bloom.Kernel: expected an
    /// enumerator name"). An enum field reads a JSON string only — an integer or any other
    /// kind is a located error, the hard cut this walker enforces from its first line.
    /// @param obj                Pointer to the destination value (default-constructed by the caller).
    /// @param type               TypeInfo carrying the field descriptors.
    /// @param value              Source JSON object.
    /// @param registry           Registry used to resolve nested struct/enum/variant/array element types.
    /// @param hooks              AssetId/Reference policy hooks; defaulted for a caller that needs neither.
    /// @param allowUnknownFields When false (the default, the cooker's strict posture), a source key
    ///                           naming no field is a located error. When true (the editor panels'
    ///                           tolerant posture), an unknown key is silently ignored.
    /// @return Empty on success; a located error string on the first malformed or (when strict) unknown field.
    VE_API VoidResult JsonReadFields(void* obj, const TypeInfo& type, const nlohmann::json& value,
                                     const TypeRegistry& registry, const JsonFieldHooks& hooks = {},
                                     bool allowUnknownFields = false);

    /// @brief Binds one JSON value into a single field's storage, per its FieldClass.
    ///
    /// The single-field seam JsonReadFields uses per key, exposed for a consumer whose values are
    /// not keyed into an enclosing object — a table cell, whose column *is* the descriptor. The
    /// accepted JSON shape and every diagnostic match JsonReadFields exactly, so a cell authors
    /// the way the same type authors as a struct field.
    /// @param fieldPtr           Pointer to the destination field storage (default-constructed by the caller).
    /// @param field              The field descriptor selecting the binding.
    /// @param value              Source JSON value.
    /// @param registry           Registry used to resolve nested struct/enum/variant/array element types.
    /// @param hooks              AssetId/Reference policy hooks; defaulted for a caller that needs neither.
    /// @param allowUnknownFields Applied to any nested struct the field recurses into.
    /// @param path               Diagnostic prefix the located error is reported under; empty for a bare value.
    /// @return Empty on success; a located error string on a malformed value.
    VE_API VoidResult JsonReadFieldValue(void* fieldPtr, const FieldDescriptor& field,
                                         const nlohmann::json& value, const TypeRegistry& registry,
                                         const JsonFieldHooks& hooks = {},
                                         bool allowUnknownFields = false, const string& path = {});

    /// @brief Writes one field's storage to a JSON value, the inverse of JsonReadFieldValue.
    /// @param fieldPtr  Pointer to the source field storage.
    /// @param field     The field descriptor selecting the encoding.
    /// @param registry  Registry used to resolve nested struct/enum/variant/array element types.
    /// @param hooks     AssetId/Reference policy hooks; defaulted for a caller that needs neither.
    /// @return The field's JSON value.
    [[nodiscard]] VE_API nlohmann::json JsonWriteFieldValue(const void* fieldPtr,
                                                            const FieldDescriptor& field,
                                                            const TypeRegistry& registry,
                                                            const JsonFieldHooks& hooks = {});

    /// @brief Writes a reflected instance to a fresh name-keyed JSON object.
    ///
    /// The write inverse of JsonReadFields, over a brand-new nlohmann::json::object() —
    /// use the merge-write overload below to patch an existing document in place instead.
    /// @param obj      Pointer to the source value.
    /// @param type     TypeInfo carrying the field descriptors.
    /// @param registry Registry used to resolve nested struct/enum/variant/array element types.
    /// @param hooks    AssetId/Reference policy hooks; defaulted for a caller that needs neither.
    /// @return A fresh JSON object keyed by field name.
    [[nodiscard]] VE_API nlohmann::json JsonWriteFields(const void* obj, const TypeInfo& type,
                                                        const TypeRegistry& registry,
                                                        const JsonFieldHooks& hooks = {});

    /// @brief Merge-write: assigns each reflected field into an existing JSON object in place.
    ///
    /// Every key `into` already carries that this walker does not own (comments-as-keys,
    /// hand-authored structure, a world id, a future field) is left untouched; only the keys
    /// named by `type`'s fields are assigned. This is the form the editor's save-in-place
    /// writers need to keep an edit a minimal diff against the hand-authored source.
    /// @param into     Destination JSON object, patched in place (created as an object first if not already one).
    /// @param obj      Pointer to the source value.
    /// @param type     TypeInfo carrying the field descriptors.
    /// @param registry Registry used to resolve nested struct/enum/variant/array element types.
    /// @param hooks    AssetId/Reference policy hooks; defaulted for a caller that needs neither.
    VE_API void JsonWriteFields(nlohmann::json& into, const void* obj, const TypeInfo& type,
                                const TypeRegistry& registry, const JsonFieldHooks& hooks = {});
}
