#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/ReflectionTypes.h>
#include <Veng/Reflection/FieldDisplay.h>

#include <functional>

namespace Veng
{
    /// @brief A condition over the owning struct that the editor evaluates per frame.
    ///
    /// The argument is the base pointer of the *immediately-enclosing* reflected struct
    /// instance — the same struct whose VE_REFLECT block declares the field. A predicate
    /// authored with VE_WHEN casts that pointer to the owning type, so handing it any other
    /// object's base is undefined behavior. An empty function is unconditional.
    ///
    /// The target is code in the registering game module's image. The host never unloads a
    /// module while its descriptors remain in the TypeRegistry — it clears the registry first,
    /// and veng has no game-code hot-reload — so the predicate outlives every call into it. It
    /// is invoked only on the single render thread, so it needs no synchronization.
    using FieldPredicate = function<bool(const void* ownerBase)>;

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
        /// @brief Per-field presentation override, merged over the field type's default by ResolveFieldDisplay.
        FieldDisplay Display;
        /// @brief When true, the editor inspector hides this field.
        bool Hidden = false;
        /// @brief When true, the editor inspector does not allow editing this field.
        bool ReadOnly = false;
        /// @brief Optional inspector category group for this field.
        string Category;
        /// @brief When set, the editor inspector shows this field only when the predicate
        /// returns true; an empty predicate is always visible.
        ///
        /// Evaluated against the owning struct's base each frame — the data-driven twin of
        /// Hidden. The serializer never touches it.
        FieldPredicate VisibleIf;
        /// @brief When set, the editor inspector allows editing this field only when the
        /// predicate returns true; an empty predicate is always enabled.
        ///
        /// Evaluated against the owning struct's base each frame — the data-driven twin of
        /// ReadOnly, composing with it (a field is editable only when both allow it). The
        /// serializer never touches it.
        FieldPredicate EnabledIf;

        /// @brief Array-only: the element type's TypeId; InvalidTypeId for a non-array field.
        ///
        /// For a FieldClass::Array field, Type names the container (a synthetic id),
        /// and this names the registered element type the array shims address.
        TypeId ElementType = InvalidTypeId;
        /// @brief Array-only: number of elements held; null for a non-array field.
        ///
        /// All four array shims take the field pointer (the `vector<T>` storage), never
        /// an element, and are null on every non-array descriptor.
        usize (*ArraySize)(const void* arrayPtr) = nullptr;
        /// @brief Array-only: returns mutable storage for the element at `index`; null for a non-array field.
        void* (*ArrayElement)(void* arrayPtr, usize index) = nullptr;
        /// @brief Array-only: returns const storage for the element at `index`; null for a non-array field.
        const void* (*ArrayElementConst)(const void* arrayPtr, usize index) = nullptr;
        /// @brief Array-only: resizes the array to `count` default-constructed elements; null for a non-array field.
        void (*ArrayResize)(void* arrayPtr, usize count) = nullptr;
    };
}
