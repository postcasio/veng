#include "FieldWidget.h"

#include "AssetChip.h"
#include "AssetSourceIndex.h"
#include "EditorIcons.h"
#include "panels/PrefabEditContext.h"

#include <Veng/Asset/Animation.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Environment.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Scene/Entity.h>
#include <Veng/UI/Inspector.h>
#include <Veng/UI/UI.h>
#include <VengEditor/EditorRegistry.h>

#include <cstring>

namespace VengEditor
{
    using namespace Veng;

    optional<AssetType> AssetTypeOfHandle(TypeId type)
    {
        if (type == TypeIdOf<AssetHandle<Texture>>())
        {
            return AssetType::Texture;
        }
        if (type == TypeIdOf<AssetHandle<Mesh>>())
        {
            return AssetType::Mesh;
        }
        if (type == TypeIdOf<AssetHandle<Material>>())
        {
            return AssetType::Material;
        }
        if (type == TypeIdOf<AssetHandle<MaterialInstance>>())
        {
            return AssetType::MaterialInstance;
        }
        if (type == TypeIdOf<AssetHandle<Prefab>>())
        {
            return AssetType::Prefab;
        }
        if (type == TypeIdOf<AssetHandle<Animation>>())
        {
            return AssetType::Animation;
        }
        if (type == TypeIdOf<AssetHandle<EnvironmentMap>>())
        {
            return AssetType::Environment;
        }
        return std::nullopt;
    }

    void ApplyAssetPick(void* fieldPtr, AssetId chosen)
    {
        const u64 value = chosen.Value;
        std::memcpy(fieldPtr, &value, sizeof(value));
    }

    namespace
    {
        // An asset chip standing in for the handle: a drop target that doubles as a
        // click-to-search selector for the field's asset type. Returns true when the pick
        // (a drop or a popup selection) changed the handle.
        bool DrawAssetPicker(void* fieldPtr, const FieldDescriptor& field, string_view label,
                             const FieldWidgetContext& ctx)
        {
            u64 currentId = 0;
            std::memcpy(&currentId, fieldPtr, sizeof(currentId));

            // A handle type the picker can't enumerate (no AssetType mapping) draws as a static
            // chip; an enumerable one is an interactive drop target / selector.
            const optional<AssetType> assetType = AssetTypeOfHandle(field.Type);

            const AssetChipInfo chip{
                .Id = AssetId{currentId},
                .Type = assetType.value_or(AssetType::Raw),
                .IdScope = label,
                .DropTarget = assetType.has_value() && !field.ReadOnly,
            };

            const optional<AssetId> picked = DrawAssetChip(chip, ctx.Sources);
            if (picked)
            {
                ApplyAssetPick(fieldPtr, *picked);
                return true;
            }
            return false;
        }

        // Reads an Entity drop on the previous widget; returns the dropped entity or nullopt.
        optional<Entity> AcceptEntityDrop()
        {
            auto target = UI::DragDropTarget();
            if (!target)
            {
                return std::nullopt;
            }
            const void* payload = UI::AcceptDragDropPayload(PrefabEditContext::EntityPayload);
            if (payload == nullptr)
            {
                return std::nullopt;
            }
            Entity dropped{};
            std::memcpy(&dropped, payload, sizeof(dropped));
            return dropped;
        }

        // A drop target plus a clear button for an intra-scene Entity reference field.
        // Returns true when the reference changed (a drop or a clear).
        bool DrawReference(void* fieldPtr, const FieldDescriptor& field, string_view label)
        {
            Entity& ref = *static_cast<Entity*>(fieldPtr);

            if (field.ReadOnly)
            {
                if (ref.IsNull())
                {
                    UI::TextDisabled("(null)");
                }
                else
                {
                    UI::TextDisabled(fmt::format("Entity {}:{}", ref.Index, ref.Generation));
                }
                return false;
            }

            bool changed = false;
            const string text = ref.IsNull()
                                    ? string{"(null)"}
                                    : fmt::format("Entity {}:{}", ref.Index, ref.Generation);
            // A label-less button is the drop surface; the id keeps it unique per field.
            (void)UI::Button(fmt::format("{}##{}", text, label));
            if (const optional<Entity> dropped = AcceptEntityDrop())
            {
                ref = *dropped;
                changed = true;
            }

            if (!ref.IsNull())
            {
                UI::SameLine();
                if (UI::IconButton(fmt::format("{}##clear{}", Icons::Remove, label)))
                {
                    ref = Entity::Null;
                    changed = true;
                }
                UI::Tooltip("Clear the reference");
            }
            return changed;
        }

        // Builds the engine inspector's hooks from the editor's dependency bundle: the asset chip
        // for AssetHandle fields, the entity drop target for Reference fields, and the
        // EditorRegistry's per-type custom widgets. The lambdas capture `ctx` by reference — the
        // hooks are consumed synchronously within the DrawFields/DrawFieldWidget call.
        UI::InspectorHooks MakeHooks(const FieldWidgetContext& ctx)
        {
            UI::InspectorHooks hooks;
            hooks.Registry = &ctx.Assets.GetTypeRegistry();
            hooks.OwnerBase = ctx.OwnerBase;
            hooks.DrawAssetHandle =
                [&ctx](void* fieldPtr, const FieldDescriptor& field, string_view valueLabel)
            { return DrawAssetPicker(fieldPtr, field, valueLabel, ctx); };
            hooks.DrawReference =
                [](void* fieldPtr, const FieldDescriptor& field, string_view valueLabel)
            { return DrawReference(fieldPtr, field, valueLabel); };
            hooks.CustomWidget = [&ctx](void* fieldPtr, const FieldDescriptor& field,
                                        string_view displayName) -> bool
            {
                const FieldWidgetFn* custom = ctx.Editors.FieldWidgetFor(field.Type);
                if (custom == nullptr)
                {
                    return false;
                }
                // A custom widget owns its whole row, including the property label.
                UI::PropertyLabel(displayName);
                (*custom)(fieldPtr, field);
                if (!field.Tooltip.empty())
                {
                    UI::Tooltip(field.Tooltip);
                }
                return true;
            };
            return hooks;
        }
    }

    bool DrawFields(void* base, std::span<const FieldDescriptor> fields,
                    const FieldWidgetContext& ctx)
    {
        return UI::DrawFields(base, fields, MakeHooks(ctx));
    }

    bool DrawFieldWidget(void* fieldPtr, const FieldDescriptor& field,
                         const FieldWidgetContext& ctx)
    {
        return UI::DrawFieldWidget(fieldPtr, field, MakeHooks(ctx));
    }
}
