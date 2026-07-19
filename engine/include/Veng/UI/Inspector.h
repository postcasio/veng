#pragma once

#include <Veng/Reflection/FieldDescriptor.h>

#include <functional>
#include <span>

namespace Veng
{
    class TypeRegistry;
}

namespace Veng::UI
{
    /// @brief Draws a leaf/reference field's value into the current property-table value column.
    ///
    /// The caller (the inspector) has already drawn the field's label and pushed its id, so the
    /// hook draws only the value widget filling column 1. Returns whether the edit changed the
    /// field this frame. Supplied by a consumer that can resolve the value beyond the field bytes
    /// — the editor's asset chip / entity-drop widgets; a bare game leaves it unset for the
    /// read-only fallback.
    using FieldValueFn =
        function<bool(void* fieldPtr, const FieldDescriptor& field, string_view valueLabel)>;

    /// @brief Draws a whole field row (label + value) for a type with a custom widget override.
    ///
    /// Called before the built-in path draws anything. When a custom widget is registered for the
    /// field's type the hook draws the property label, the custom value, and any tooltip itself,
    /// then returns true so the inspector skips the built-in widget; when none is registered it
    /// draws nothing and returns false, so the inspector falls through to the built-in. The void
    /// custom-widget signature carries no change signal, so a custom-widget edit reports no change.
    using CustomWidgetFn =
        function<bool(void* fieldPtr, const FieldDescriptor& field, string_view displayName)>;

    /// @brief Consumer-supplied dependencies the reflection inspector needs beyond field bytes.
    ///
    /// A bare game supplies only `Registry`; the drawing hooks stay unset and the inspector draws
    /// its read-only fallbacks for AssetHandle/Reference fields. The editor supplies all three to
    /// get asset chips, entity-drop references, and per-type custom widgets.
    struct InspectorHooks
    {
        /// @brief Type registry used for enum/struct/variant/array recursion. Required.
        const TypeRegistry* Registry = nullptr;
        /// @brief Base of the struct the current field walk iterates, for VisibleIf/EnabledIf.
        ///
        /// Re-seeded at each walk level (top-level struct, nested struct, array element) so a
        /// field's predicate evaluates against its immediate owner; null disables every condition
        /// (a field with no predicate is unaffected either way).
        const void* OwnerBase = nullptr;
        /// @brief Draws a FieldClass::AssetHandle value; unset draws a read-only id label.
        FieldValueFn DrawAssetHandle;
        /// @brief Draws a FieldClass::Reference value; unset draws a read-only "(reference)" label.
        FieldValueFn DrawReference;
        /// @brief Draws a per-type custom widget row; unset (or no match) uses the built-in widget.
        CustomWidgetFn CustomWidget;
    };

    /// @brief Walks a struct's (or component's) fields as property-table rows, grouping by Category.
    ///
    /// The single field-walk every reflection inspector surface routes through. Skips `Hidden`
    /// fields and calls `DrawFieldWidget` for each, with `fieldPtr` derived as
    /// `base + FieldDescriptor::Offset`. Fields carrying a `Category` are grouped under a
    /// full-width `UI::PropertyHeader` named for the category; un-categorized fields render first.
    /// Grouping is stable: declared order within a category, categories in first-seen order. Each
    /// field is gated by its VisibleIf (a failing one skips the row) and EnabledIf (a failing one
    /// disables the row, composing with ReadOnly), both evaluated against `base` — which the
    /// helper sets as the walk's owner base.
    /// @param base   Pointer to the owning struct/component instance.
    /// @param fields The owning type's field descriptors, in declared order.
    /// @param hooks  Registry + owner base + the consumer's drawing hooks.
    /// @return True when any field's edit changed it, so a caller can re-resolve the owner.
    /// @pre Called inside an open `UI::PropertyTable` scope.
    bool DrawFields(void* base, std::span<const FieldDescriptor> fields,
                    const InspectorHooks& hooks);

    /// @brief Draws one field as a property-table row: label in column 0, value in column 1.
    ///
    /// Applies a registered custom widget when the hooks provide one for the field's type;
    /// otherwise uses the per-FieldClass built-in widget
    /// (Scalar/Vector/Quaternion/String/AssetHandle/Enum/Reference/Struct/Matrix/Variant/Array).
    /// A nested struct flattens into further indented rows of the same table (no nested table).
    /// Respects `ReadOnly` (disabled or read-only value), `Hidden` (skipped), and `Tooltip`.
    /// @param fieldPtr Pointer to the field bytes (base + FieldDescriptor::Offset).
    /// @param field    Descriptor giving the field's type, class, and metadata.
    /// @param hooks    Registry + owner base + the consumer's drawing hooks.
    /// @return True when this edit changed the field (including any nested/variant member).
    /// @pre Called inside an open `UI::PropertyTable` scope.
    bool DrawFieldWidget(void* fieldPtr, const FieldDescriptor& field, const InspectorHooks& hooks);

    /// @brief Draws only a field's value widget, with no label and no property-table row.
    ///
    /// The same per-FieldClass widget DrawFieldWidget draws in a row's value column, minus the
    /// label — what a caller laying its own grid out needs, where the column header already names
    /// the value and a row advance would break the layout. A table cell is the motivating case: it
    /// draws the widget for the column's reflected type, so an enum cell gets the named combo and
    /// an asset-handle cell the picker, with no per-cell widget written.
    ///
    /// Composite classes (Struct, Variant, Array) draw nothing and return false: they expand into
    /// several rows and cannot render as a single value. A caller wanting those must open a
    /// property table and use DrawFieldWidget.
    /// @param fieldPtr Pointer to the field bytes.
    /// @param field    Descriptor giving the field's type, class, and metadata.
    /// @param hooks    Registry + owner base + the consumer's drawing hooks.
    /// @return True when this edit changed the value.
    bool DrawFieldValue(void* fieldPtr, const FieldDescriptor& field, const InspectorHooks& hooks);
}
