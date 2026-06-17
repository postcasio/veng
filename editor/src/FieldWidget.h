#pragma once

#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>

namespace Veng
{
    class AssetManager;
    class EditorRegistry;
}

namespace VengEditor
{
    class AssetSourceIndex;

    // The asset type a FieldClass::AssetHandle field references, derived from its
    // leaf TypeId — the filter the picker enumerates manifest candidates by.
    // nullopt for a handle type the picker offers no enumeration for.
    [[nodiscard]] Veng::optional<Veng::AssetType> AssetTypeOfHandle(Veng::TypeId type);

    // Write a picked AssetId back through an AssetHandle field pointer: the id is
    // the leading u64 of any AssetHandle<T> (offset 0 is pinned by AssetHandle's
    // layout guard), so it is writable without naming the concrete asset type.
    // The single write-back the picker's combo selection performs.
    void ApplyAssetPick(void* fieldPtr, Veng::AssetId chosen);

    // The dependencies a field widget needs beyond the field bytes themselves:
    // the asset manager (handle widgets resolve the referenced asset), the source
    // index (the AssetHandle picker's type-filtered candidate enumeration), and
    // the editor registry (RegisterFieldWidget overrides + struct recursion).
    struct FieldWidgetContext
    {
        Veng::AssetManager& Assets;
        const AssetSourceIndex& Sources;
        const Veng::EditorRegistry& Editors;
    };

    // Draws one field's inspector widget into the live ImGui frame: the per-
    // FieldClass built-in widget, a registered RegisterFieldWidget override when
    // one exists for the field's TypeId, and a recursion into a nested struct's
    // own fields. The entity inspector and the node-property inspector both call
    // this, so the two share identical widget behavior. fieldPtr addresses the
    // field's bytes (base + FieldDescriptor::Offset); the widget mutates them in
    // place.
    void DrawFieldWidget(void* fieldPtr, const Veng::FieldDescriptor& field,
                         const FieldWidgetContext& ctx);
}
