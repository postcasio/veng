#pragma once

#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>

#include <span>

namespace Veng
{
    class AssetManager;
    class EditorRegistry;
}

namespace VengEditor
{
    class AssetSourceIndex;

    /// @brief Returns the AssetType a FieldClass::AssetHandle field references, or
    /// nullopt for a handle type the picker has no enumeration for.
    ///
    /// Used to filter manifest candidates in the asset picker.
    [[nodiscard]] Veng::optional<Veng::AssetType> AssetTypeOfHandle(Veng::TypeId type);

    /// @brief Writes a chosen AssetId back through an AssetHandle field pointer.
    ///
    /// The id occupies the leading u64 of any AssetHandle<T> (pinned at offset 0
    /// by AssetHandle's layout guard), so it is writable without naming the
    /// concrete asset type.
    /// @param fieldPtr Pointer to the AssetHandle field bytes.
    /// @param chosen   The selected asset id to write.
    void ApplyAssetPick(void* fieldPtr, Veng::AssetId chosen);

    /// @brief Dependencies a field widget needs beyond the field bytes themselves.
    struct FieldWidgetContext
    {
        /// @brief Asset manager for resolving handle widgets.
        Veng::AssetManager& Assets;
        /// @brief Source index for type-filtered candidate enumeration in the asset picker.
        const AssetSourceIndex& Sources;
        /// @brief Editor registry for RegisterFieldWidget overrides and struct recursion.
        const Veng::EditorRegistry& Editors;
        /// @brief Base pointer of the struct whose fields the current `DrawFields` walk iterates.
        ///
        /// Re-seeded at each walk level (top-level component, nested struct, array element) so a
        /// field's VisibleIf/EnabledIf predicate evaluates against its immediate owner. The four
        /// panel walks seed it with the struct/component they iterate; null disables every
        /// condition (a field with no predicate is unaffected either way).
        const void* OwnerBase = nullptr;
    };

    /// @brief Draws one field as a property-table row: label in column 0, value in column 1.
    ///
    /// Emits a `UI::PropertyLabel(displayName)` then a label-less value widget filling the
    /// value column, so the caller must be inside a `UI::PropertyTable` scope. Applies a
    /// RegisterFieldWidget override when one is registered for the field's TypeId; otherwise
    /// uses the per-FieldClass built-in widget. A nested struct flattens into further indented
    /// rows of the same table (no nested table). Respects `ReadOnly` (disabled or read-only
    /// value), `Hidden` (skipped), and `Tooltip`. Both the entity inspector and the
    /// node-property inspector call this function, so they share identical widget behavior.
    /// @param fieldPtr Pointer to the field bytes (base + FieldDescriptor::Offset).
    /// @param field    Descriptor giving the field's type, class, and metadata.
    /// @param ctx      Dependencies: asset manager, source index, editor registry.
    /// @return True when this edit changed the field (including any nested/variant member),
    ///         so a caller can re-resolve a touched resolver-bearing component.
    /// @pre Called inside an open `UI::PropertyTable` scope.
    bool DrawFieldWidget(void* fieldPtr, const Veng::FieldDescriptor& field,
                         const FieldWidgetContext& ctx);

    /// @brief Walks a struct's (or component's) fields as property-table rows, grouping by Category.
    ///
    /// The single field-walk every inspector surface routes through: the entity inspector, the
    /// node-property inspector, project settings, the level editor, and the nested-struct
    /// recursion. Skips `Hidden` fields and calls `DrawFieldWidget` for each, with `fieldPtr`
    /// derived as `base + FieldDescriptor::Offset`. Fields carrying a `Category` are grouped
    /// under a full-width `UI::PropertyHeader` named for the category; un-categorized fields
    /// render first. Grouping is stable: declared order within a category, categories in
    /// first-seen order. Each field is gated by its VisibleIf (a failing one skips the row) and
    /// EnabledIf (a failing one disables the row, composing with ReadOnly), both evaluated
    /// against `base` — which the helper sets as the walk's owner base.
    /// @param base    Pointer to the owning struct/component instance.
    /// @param fields  The owning type's field descriptors, in declared order.
    /// @param ctx     Dependencies: asset manager, source index, editor registry.
    /// @return True when any field's edit changed it, so a caller can re-resolve the owner.
    /// @pre Called inside an open `UI::PropertyTable` scope.
    bool DrawFields(void* base, std::span<const Veng::FieldDescriptor> fields,
                    const FieldWidgetContext& ctx);
}
