#include "FieldWidget.h"

#include "AssetSourceIndex.h"

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Entity.h>
#include <Veng/UI/UI.h>
#include <VengEditor/EditorRegistry.h>

#include <cstring>

namespace VengEditor
{
    using namespace Veng;

    optional<AssetType> AssetTypeOfHandle(TypeId type)
    {
        if (type == TypeIdOf<AssetHandle<Texture>>()) return AssetType::Texture;
        if (type == TypeIdOf<AssetHandle<Mesh>>()) return AssetType::Mesh;
        if (type == TypeIdOf<AssetHandle<Material>>()) return AssetType::Material;
        return std::nullopt;
    }

    void ApplyAssetPick(void* fieldPtr, AssetId chosen)
    {
        const u64 value = chosen.Value;
        std::memcpy(fieldPtr, &value, sizeof(value));
    }

    namespace
    {
        // The AssetHandle picker: a combo over the manifest's ids of the field's
        // asset type, writing the chosen id back through the leading u64 of the
        // handle (offset 0 is pinned by AssetHandle's layout guard, so the id is
        // writable without naming the asset's concrete type). "(none)" clears it.
        void DrawAssetPicker(void* fieldPtr, const FieldDescriptor& field, string_view label,
                             const FieldWidgetContext& ctx)
        {
            u64 currentId = 0;
            std::memcpy(&currentId, fieldPtr, sizeof(currentId));

            const optional<AssetType> assetType = AssetTypeOfHandle(field.Type);
            if (!assetType)
            {
                // No enumeration for this handle type — fall back to the id label.
                if (currentId == 0)
                    UI::Label(label, "(none)");
                else
                    UI::Label(label, fmt::format("0x{:X}", currentId));
                return;
            }

            // The combo entries are "(none)" first, then one per candidate id; the
            // selected index addresses this same list, so index 0 clears the handle
            // and index N picks the candidate at N-1.
            const vector<AssetId> candidates = ctx.Sources.EntriesOfType(*assetType);

            vector<string> labels;
            labels.reserve(candidates.size() + 1);
            labels.emplace_back("(none)");
            for (const AssetId candidate : candidates)
                labels.push_back(fmt::format("0x{:X}", candidate.Value));

            vector<string_view> items(labels.begin(), labels.end());

            i32 index = 0;
            for (usize i = 0; i < candidates.size(); ++i)
            {
                if (candidates[i].Value == currentId)
                {
                    index = static_cast<i32>(i) + 1;
                    break;
                }
            }

            if (UI::Combo(label, index, items))
            {
                if (index == 0)
                    ApplyAssetPick(fieldPtr, AssetId{});
                else
                    ApplyAssetPick(fieldPtr, candidates[static_cast<usize>(index) - 1]);
            }
        }
    }

    void DrawFieldWidget(void* fieldPtr, const FieldDescriptor& field, const FieldWidgetContext& ctx)
    {
        if (field.Hidden)
            return;

        // A game-registered custom widget for this field's type overrides the
        // built-in chosen from FieldClass.
        if (const FieldWidgetFn* custom = ctx.Editors.FieldWidgetFor(field.Type))
        {
            (*custom)(fieldPtr, field);
            return;
        }

        const string& label = field.DisplayName.empty() ? field.Name : field.DisplayName;

        auto id = UI::PushId(label);

        // The field's optional editor metadata is the single source of drag speed
        // and clamp range; absent metadata leaves the DragOptions defaults
        // (0.01f speed, unclamped).
        UI::DragOptions drag;
        if (field.Step) drag.Speed = static_cast<f32>(*field.Step);
        if (field.Min) drag.Min = static_cast<f32>(*field.Min);
        if (field.Max) drag.Max = static_cast<f32>(*field.Max);

        switch (field.Class)
        {
        case FieldClass::Scalar:
        {
            if (field.Type == TypeIdOf<f32>())
            {
                UI::Drag(label, *static_cast<f32*>(fieldPtr), drag);
            }
            else if (field.Type == TypeIdOf<i32>())
            {
                UI::Drag(label, *static_cast<i32*>(fieldPtr));
            }
            else if (field.Type == TypeIdOf<u32>())
            {
                // u32 has no Drag overload; edit through a signed view clamped at 0
                // so the unsigned field never receives a negative value.
                i32 value = static_cast<i32>(*static_cast<u32*>(fieldPtr));
                if (UI::Drag(label, value, UI::DragOptions{ .Min = 0.0f }))
                    *static_cast<u32*>(fieldPtr) = static_cast<u32>(value < 0 ? 0 : value);
            }
            else if (field.Type == TypeIdOf<bool>())
            {
                UI::Checkbox(label, *static_cast<bool*>(fieldPtr));
            }
            else
            {
                UI::Label(label, "(scalar)");
            }
            break;
        }
        case FieldClass::Vector:
        {
            if (field.Type == TypeIdOf<vec2>())
                UI::Drag(label, *static_cast<vec2*>(fieldPtr), drag);
            else if (field.Type == TypeIdOf<vec3>())
                UI::Drag(label, *static_cast<vec3*>(fieldPtr), drag);
            else if (field.Type == TypeIdOf<vec4>())
                UI::Drag(label, *static_cast<vec4*>(fieldPtr), drag);
            break;
        }
        case FieldClass::Quaternion:
        {
            quat& q = *static_cast<quat*>(fieldPtr);
            vec3 euler = glm::degrees(glm::eulerAngles(q));
            const string eulerLabel = label + " (Euler °)";
            if (UI::Drag(eulerLabel, euler, UI::DragOptions{ .Speed = 0.5f }))
                q = quat(glm::radians(euler));
            break;
        }
        case FieldClass::String:
        {
            // UI::InputText owns the commit-on-deactivate semantics and writes the
            // value back internally; it returns true only on a fresh commit.
            string& value = *static_cast<string*>(fieldPtr);
            UI::InputText(label, value);
            break;
        }
        case FieldClass::AssetHandle:
        {
            DrawAssetPicker(fieldPtr, field, label, ctx);
            break;
        }
        case FieldClass::Reference:
        {
            const Entity& ref = *static_cast<const Entity*>(fieldPtr);
            if (ref.IsNull())
                UI::Label(label, "(null)");
            else
                UI::Label(label, fmt::format("Entity {}:{}", ref.Index, ref.Generation));
            break;
        }
        case FieldClass::Matrix:
        {
            const mat4& m = *static_cast<const mat4*>(fieldPtr);
            if (auto t = UI::TreeNode(label, UI::TreeFlags::SpanAvailWidth))
            {
                for (int row = 0; row < 4; ++row)
                    UI::Text(fmt::format("{: .3f}  {: .3f}  {: .3f}  {: .3f}",
                                         m[0][row], m[1][row], m[2][row], m[3][row]));
            }
            break;
        }
        case FieldClass::Enum:
        {
            // No enum-value table is recorded in the reflection layer; show the
            // backing integer read-only until a value table exists.
            const i32 value = *static_cast<const i32*>(fieldPtr);
            UI::Label(label, fmt::format("{}", value));
            break;
        }
        case FieldClass::Struct:
        {
            const TypeInfo& nested = ctx.Assets.GetTypeRegistry().Info(field.Type);
            if (auto t = UI::TreeNode(label, UI::TreeFlags::SpanAvailWidth))
            {
                for (const FieldDescriptor& nestedField : nested.Fields)
                {
                    if (nestedField.Hidden)
                        continue;
                    void* nestedPtr = static_cast<u8*>(fieldPtr) + nestedField.Offset;
                    DrawFieldWidget(nestedPtr, nestedField, ctx);
                }
            }
            break;
        }
        }

        if (!field.Tooltip.empty())
            UI::Tooltip(field.Tooltip);
    }
}
