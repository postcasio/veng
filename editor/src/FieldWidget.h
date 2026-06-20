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
    };

    /// @brief Draws one field's inspector widget into the live ImGui frame.
    ///
    /// Applies a RegisterFieldWidget override when one is registered for the field's
    /// TypeId; otherwise uses the per-FieldClass built-in widget, recursing into
    /// nested structs. Both the entity inspector and the node-property inspector
    /// call this function, so they share identical widget behavior.
    /// @param fieldPtr Pointer to the field bytes (base + FieldDescriptor::Offset).
    /// @param field    Descriptor giving the field's type, class, and metadata.
    /// @param ctx      Dependencies: asset manager, source index, editor registry.
    void DrawFieldWidget(void* fieldPtr, const Veng::FieldDescriptor& field,
                         const FieldWidgetContext& ctx);
}
