#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeId.h>

namespace Veng
{
    // The reflected description of one field of a struct. The first four members
    // are the serialization triple-plus-offset the generic walker reads; the rest
    // is editor metadata the serializer never touches.
    struct FieldDescriptor
    {
        // The serialization key — stable and code-facing. Field identity within a
        // type is the name; it is never the human UI label (that is DisplayName),
        // so relabelling in the editor never breaks on-disk compatibility.
        string Name;
        // The field's leaf / struct / component type, read off its trait at
        // compile time.
        TypeId Type = InvalidTypeId;
        // Denormalised from the field type's trait so the walker handles a leaf
        // field without a registry lookup.
        FieldClass Class = FieldClass::Scalar;
        // offsetof(Owner, member).
        usize Offset = 0;

        // Editor metadata — optional, default-empty; the serializer ignores all
        // of it. DisplayName falls back to Name when empty.
        string DisplayName;
        string Tooltip;
        optional<f64> Min;
        optional<f64> Max;
        optional<f64> Step;
        bool Hidden = false;
        bool ReadOnly = false;
        string Category;
    };
}
