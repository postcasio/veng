#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/ReflectionTypes.h>

namespace Veng
{
    /// @brief The reflected description of one field of a struct.
    ///
    /// The first four members are the serialization triple-plus-offset the generic
    /// walker reads; the rest is editor metadata the serializer never touches.
    struct FieldDescriptor
    {
        /// @brief Serialization key — stable and code-facing.
        ///
        /// Field identity within a type is the name, never the human UI label
        /// (that is DisplayName), so relabelling in the editor never breaks
        /// on-disk compatibility.
        string Name;
        /// @brief The field's leaf/struct/component type, read off its trait at compile time.
        TypeId Type = InvalidTypeId;
        /// @brief Meta-kind, denormalised from the field type's trait so the walker avoids a registry lookup per leaf.
        FieldClass Class = FieldClass::Scalar;
        /// @brief `offsetof(Owner, member)`.
        usize Offset = 0;

        /// @brief Human-readable label shown in the editor; falls back to Name when empty.
        ///
        /// The serializer ignores all editor metadata fields below.
        string DisplayName;
        /// @brief Editor tooltip text.
        string Tooltip;
        /// @brief Optional minimum value hint for editor widgets.
        optional<f64> Min;
        /// @brief Optional maximum value hint for editor widgets.
        optional<f64> Max;
        /// @brief Optional step size hint for editor drag widgets.
        optional<f64> Step;
        /// @brief When true, the editor inspector hides this field.
        bool Hidden = false;
        /// @brief When true, the editor inspector does not allow editing this field.
        bool ReadOnly = false;
        /// @brief Optional inspector category group for this field.
        string Category;
    };
}
