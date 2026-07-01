#pragma once

#include <Veng/Veng.h>

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
}
